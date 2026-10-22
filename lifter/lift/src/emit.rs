//! IR -> C emitter for one function.

use crate::absint::AbsRes;
use crate::flagc::*;
use crate::flags::FlagPlan;
use ir::*;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::fmt::Write;

/// Summary of a callee as needed by call sites.
#[derive(Clone, Debug, Default)]
pub struct Sig {
    /// extended inputs: bit mask ebx=1 esi=2 edi=4 ebp=8
    pub ext_in: u8,
    /// extended outputs: ebx=1 esi=2 edi=4 ebp=8 ecx=16
    pub ext_out: u8,
    /// constant `ret N` (None = dynamic)
    pub pop: Option<u16>,
    /// callee reads/modifies its return slot: callers must store the return address
    pub ret_addr: bool,
    /// returns a value in st(0)
    pub st0_ret: bool,
    pub noreturn: bool,
    /// reads ecx at entry (thiscall-like)
    pub ecx_in: bool,
}

pub trait Callees {
    fn sig(&self, addr: u32) -> Sig;
    /// native override (host symbol, kind)
    fn ovr(&self, addr: u32) -> Option<(String, crate::ctx::OvrKind)>;
    fn ovr_pop(&self, addr: u32) -> Option<u16>;
    fn sym_index(&self, host: &str) -> u32;
    fn sym_name(&self, idx: u32) -> String;
}

pub fn gname(addr: u32) -> String {
    format!("g_{:08X}", addr)
}

pub const EXT_ORDER: [Gpr; 4] = [Gpr::Ebx, Gpr::Esi, Gpr::Edi, Gpr::Ebp];

pub fn ext_bit(g: Gpr) -> u8 {
    match g {
        Gpr::Ebx => 1,
        Gpr::Esi => 2,
        Gpr::Edi => 4,
        Gpr::Ebp => 8,
        _ => 0,
    }
}

pub struct Emitter<'a> {
    pub f: &'a Func,
    pub plan: &'a FlagPlan,
    pub abs: &'a AbsRes,
    pub me: Sig,
    pub callees: &'a dyn Callees,
    pub out: String,
    pub cons_rhs: HashMap<(u32, u32), String>,
    /// per class: used lazy vars / materialised bits
    pub lazy_used: Vec<[bool; 5]>, // a,b,r,ci,cn
    pub mat_used: Vec<u8>,
    pub cur_blk: u32,
    pub n_blocks: u32,
    pub uses_df_dyn: bool,
    pub uses_teb: bool,
    pub uses_gt: bool,
    /// the function has an EH landing pad (Op::ResumeSetup)
    pub uses_pad: bool,
    pub uses_mb: bool,
    pub need_esp0: bool,
    pub gpr_used: [bool; 8],
    pub xmm_used: [bool; 8],
    pub fp_slots: u8,
    pub uses_fsw: bool,
    pub block_target: Vec<bool>,
    pub n_traps: Vec<(u32, u16)>,
    pub fp_cc_used: bool,
    /// referenced lifted functions: (addr, extended-form)
    pub refs: BTreeSet<(u32, bool)>,
    pub ovr_refs: BTreeSet<String>,
    pub x87_host_refs: BTreeSet<String>,
    pub entry_addr: u32,
    pub trace: bool,
}

pub fn ty_c(ty: Ty) -> &'static str {
    cty(ty)
}

impl<'a> Emitter<'a> {
    pub fn new(f: &'a Func, plan: &'a FlagPlan, abs: &'a AbsRes, me: Sig, callees: &'a dyn Callees) -> Emitter<'a> {
        let ncl = plan.classes.len();
        Emitter {
            f,
            plan,
            abs,
            me,
            callees,
            out: String::new(),
            cons_rhs: HashMap::new(),
            lazy_used: vec![[false; 5]; ncl],
            mat_used: vec![0; ncl],
            cur_blk: 0,
            n_blocks: f.blocks.len() as u32,
            uses_df_dyn: false,
            uses_teb: false,
            uses_gt: false,
            uses_pad: false,
            uses_mb: false,
            need_esp0: false,
            gpr_used: [false; 8],
            xmm_used: [false; 8],
            fp_slots: 0,
            uses_fsw: false,
            block_target: vec![false; f.blocks.len()],
            n_traps: Vec::new(),
            fp_cc_used: false,
            refs: BTreeSet::new(),
            ovr_refs: BTreeSet::new(),
            x87_host_refs: BTreeSet::new(),
            entry_addr: f.entry,
            trace: false,
        }
    }

    // ------------------------------------------------------------------ operand text
    pub fn place(&self, p: &Place) -> String {
        match p {
            Place::Gpr(g) => g.name().to_string(),
            Place::Tmp(t) => format!("t{t}"),
            Place::Xmm(i) => format!("xmm{i}"),
            Place::Mm(i) => format!("xmm{}", 8 + *i as u32),
            Place::St(i) => format!("st_rel{i}"),
            Place::Fp(i) => format!("f{i}"),
        }
    }
    pub fn imm(&self, v: u64, ty: Ty) -> String {
        match ty {
            Ty::I1 => format!("{}", v & 1),
            Ty::I64 => format!("{:#x}ull", v),
            Ty::F32 => format!("lx_f32_from_bits({:#x}u)", v as u32),
            Ty::F64 => format!("lx_f64_from_bits({:#x}ull)", v),
            _ => format!("{:#x}u", v & ty.mask()),
        }
    }
    pub fn opnd(&self, o: &Opnd, ty: Ty) -> String {
        match o {
            Opnd::P(p) => self.place(p),
            Opnd::Imm(v) => self.imm(*v, ty),
        }
    }
    /// 32-bit guest address expression
    pub fn addr(&self, m: &Mem) -> String {
        let mut parts: Vec<String> = Vec::new();
        if m.fs {
            parts.push("teb".into());
        }
        if let Some(b) = &m.base {
            parts.push(self.place(b));
        }
        if let Some(x) = &m.index {
            let xs = self.place(x);
            parts.push(match m.scale {
                1 => xs,
                s => format!("{xs} * {s}u"),
            });
        }
        let d = m.disp as u32;
        if parts.is_empty() {
            return format!("{:#x}u", d);
        }
        let mut s = parts.join(" + ");
        if d != 0 {
            if (d as i32) < 0 && (d as i32) > -0x10000 {
                s = format!("{s} - {:#x}u", (d as i32).wrapping_neg() as u32);
            } else {
                s = format!("{s} + {:#x}u", d);
            }
        }
        s
    }

    fn is_local(&self, m: &Mem) -> bool {
        m.fs || m.cls == MemCls::Local
    }

    /// load expression of `bytes` unsigned integer width (1,2,4,8) honouring class and alignment
    pub fn ld_int(&self, bytes: u32, m: &Mem) -> String {
        let t = match bytes {
            1 => "uint8_t",
            2 => "uint16_t",
            4 => "uint32_t",
            _ => "uint64_t",
        };
        let a = self.addr(m);
        if self.is_local(m) {
            format!("GUEST_LD_LOCAL({t}, {a})")
        } else if m.aligned || bytes == 1 {
            format!("GUEST_LD_SHARED({t}, {a})")
        } else {
            format!("LX_LD_SU{bytes}({a})")
        }
    }
    pub fn st_int(&self, bytes: u32, m: &Mem, v: &str) -> String {
        let t = match bytes {
            1 => "uint8_t",
            2 => "uint16_t",
            4 => "uint32_t",
            _ => "uint64_t",
        };
        let a = self.addr(m);
        if self.is_local(m) {
            format!("GUEST_ST_LOCAL({t}, {a}, {v});")
        } else if m.aligned || bytes == 1 {
            format!("GUEST_ST_SHARED({t}, {a}, {v});")
        } else {
            format!("LX_ST_SU{bytes}({a}, {v});")
        }
    }

    pub fn line(&mut self, s: &str) {
        self.out.push_str("    ");
        self.out.push_str(s);
        self.out.push('\n');
    }

    /// statement assigning `rhs` to place `d` (all temporaries are declared at the top of the function)
    pub fn assign(&mut self, d: &Place, _ty: Ty, rhs: &str) {
        let s = format!("{} = {};", self.place(d), rhs);
        self.line(&s);
    }
    pub fn assign_inline(&mut self, d: &Place, rhs: &str) {
        let s = format!("{} = {};", self.place(d), rhs);
        self.line(&s);
    }

    // ------------------------------------------------------------------ prepasses
    pub fn prepass(&mut self) {
        let f = self.f;
        for (bi, b) in f.blocks.iter().enumerate() {
            for (ii, ins) in b.insts.iter().enumerate() {
                ins.op.visit_uses(&mut |p| {
                    if let Place::Gpr(g) = p {
                        self.gpr_used[g as usize] = true;
                    }
                });
                ins.op.visit_defs(&mut |p| {
                    if let Place::Gpr(g) = p {
                        self.gpr_used[g as usize] = true;
                    }
                });
                match &ins.op {
                    Op::Ld { m, .. } | Op::St { m, .. } | Op::AtomicRmw { m, .. } | Op::AtomicCas { m, .. } | Op::FLdM { m, .. } | Op::FStM { m, .. } | Op::VLd { m, .. } | Op::VSt { m, .. } => {
                        self.uses_mb = true;
                        if let Op::FStM { fmt: FMem::I16 | FMem::I32 | FMem::I64, trunc: false, .. } = &ins.op {
                            self.uses_gt = true;
                        }
                        if m.fs {
                            self.uses_teb = true;
                        }
                    }
                    Op::Str { .. } | Op::Misc(_) => self.uses_mb = true,
                    Op::Call { target: CallTarget::Direct(a), .. } => {
                        self.uses_mb = true;
                        let sg = self.callees.sig(*a);
                        for g in EXT_ORDER {
                            if (sg.ext_in | sg.ext_out) & ext_bit(g) != 0 {
                                self.gpr_used[g as usize] = true;
                            }
                        }
                    }
                    Op::Call { .. } => self.uses_mb = true,
                    Op::SetDf(o) => {
                        if !matches!(o, Opnd::Imm(0)) {
                            self.uses_df_dyn = true;
                        }
                    }
                    Op::FRetLoad { .. } | Op::FRetStore { .. } | Op::FLdcw { .. } | Op::FNstcw { .. } | Op::FInit | Op::FUn { op: FUn::Rndint, .. } => self.uses_gt = true,
                    _ => {}
                }
                let _ = (bi, ii);
            }
            b.term.visit_uses(&mut |p| {
                if let Place::Gpr(g) = p {
                    self.gpr_used[g as usize] = true;
                }
            });
            for s in b.term.succs() {
                self.block_target[s as usize] = true;
            }
            if let Term::TailCall(CallTarget::Direct(a)) = &b.term {
                let sg = self.callees.sig(*a);
                for g in EXT_ORDER {
                    if (sg.ext_in | sg.ext_out) & ext_bit(g) != 0 {
                        self.gpr_used[g as usize] = true;
                    }
                }
            }
        }
        if self.uses_teb {
            self.uses_gt = true;
        }
        for g in EXT_ORDER {
            if self.me.ext_out & ext_bit(g) != 0 {
                self.gpr_used[g as usize] = true;
            }
        }
    }

    // ------------------------------------------------------------------ flags
    fn class_var(&self, c: i32, s: &str) -> String {
        format!("fc{c}_{s}")
    }

    /// RHS text for a flag consumer; records variable usage
    pub fn consumer_rhs(&mut self, key: (u32, u32), op: &Op) -> String {
        let info = &self.plan.cons[&key];
        let bc = info.bit_class;
        let bit = |this: &mut Self, k: usize| -> String {
            let c = bc[k];
            if c < 0 {
                return "0".into();
            }
            let cl = &this.plan.classes[c as usize];
            if let Some((kind, ty)) = cl.lazy {
                let names = [format!("fc{c}_a"), format!("fc{c}_b"), format!("fc{c}_r"), format!("fc{c}_ci"), format!("fc{c}_cn")];
                let v = V { a: &names[0], b: &names[1], r: &names[2], ci: &names[3], cn: &names[4] };
                match bit_formula(kind, ty, k, &v) {
                    Some(s) => {
                        for (i, n) in names.iter().enumerate() {
                            if s.contains(n.as_str()) {
                                this.lazy_used[c as usize][i] = true;
                            }
                        }
                        s
                    }
                    None => "0".into(),
                }
            } else {
                this.mat_used[c as usize] |= 1 << k;
                format!("fc{c}_{}", ["cf", "pf", "af", "zf", "sf", "of"][k])
            }
        };
        match op {
            Op::FlagGet { cond, .. } => {
                let need = cond.flags();
                // fused?
                let mut cls: Option<i32> = None;
                let mut same = true;
                for k in 0..6 {
                    if need & (1 << k) != 0 {
                        match cls {
                            None => cls = Some(bc[k]),
                            Some(c) => {
                                if c != bc[k] {
                                    same = false
                                }
                            }
                        }
                    }
                }
                if let (true, Some(c)) = (same, cls) {
                    if c >= 0 {
                        if let Some((kind, ty)) = self.plan.classes[c as usize].lazy {
                            let names = [format!("fc{c}_a"), format!("fc{c}_b"), format!("fc{c}_r"), format!("fc{c}_ci"), format!("fc{c}_cn")];
                            let v = V { a: &names[0], b: &names[1], r: &names[2], ci: &names[3], cn: &names[4] };
                            if let Some(s) = fused_cond(kind, ty, *cond, &v) {
                                for (i, n) in names.iter().enumerate() {
                                    if s.contains(n.as_str()) {
                                        self.lazy_used[c as usize][i] = true;
                                    }
                                }
                                // mark bits as used for bookkeeping
                                return s;
                            }
                        }
                    }
                }
                let cc = *cond;
                let mut bits: Vec<String> = Vec::new();
                for k in 0..6 {
                    bits.push(if need & (1 << k) != 0 { bit(self, k) } else { "0".into() });
                }
                combine(cc, &|k| bits[k].clone())
            }
            Op::FlagGetBit { flag, .. } => {
                let k = flag.trailing_zeros() as usize;
                let b = bit(self, k);
                format!("(uint32_t){b}")
            }
            Op::FlagsPack { .. } => {
                let mut s = String::from("0x202u");
                for k in 0..6 {
                    let b = bit(self, k);
                    if b != "0" {
                        s.push_str(&format!(" | ((uint32_t){b} << {}u)", FL_POS[k]));
                    }
                }
                if self.uses_df_dyn {
                    s.push_str(" | (df << 10)");
                }
                s
            }
            _ => unreachable!(),
        }
    }

    fn emit_flagdef(&mut self, key: (u32, u32), d: &FlagDef) {
        let pid = match self.plan.prod_at.get(&key) {
            Some(p) => *p,
            None => return,
        };
        let cid = self.plan.class_of[pid as usize - 1];
        if cid < 0 {
            return; // dead producer
        }
        let cl = &self.plan.classes[cid as usize];
        let ty = d.ty;
        let cnt = if d.cond_cnt { Some(self.opnd(&d.cnt, Ty::I32)) } else { None };
        let mut stmts: Vec<String> = Vec::new();
        if let Some((_kind, _)) = cl.lazy {
            let used = self.lazy_used[cid as usize];
            let t = cty(ty);
            if used[0] {
                stmts.push(format!("fc{cid}_a = ({t}){};", self.opnd(&d.a, ty)));
            }
            if used[1] {
                stmts.push(format!("fc{cid}_b = ({t}){};", self.opnd(&d.b, ty)));
            }
            if used[2] {
                stmts.push(format!("fc{cid}_r = ({t}){};", self.opnd(&d.r, ty)));
            }
            if used[3] {
                stmts.push(format!("fc{cid}_ci = {};", self.opnd(&d.cin, Ty::I32)));
            }
            if used[4] {
                stmts.push(format!("fc{cid}_cn = {};", self.opnd(&d.cnt, Ty::I32)));
            }
        } else {
            let used = self.mat_used[cid as usize];
            let t = cty(ty);
            // materialise the consumed bits that this producer defines
            let (a, b, r, ci, cn) = (
                format!("(({t}){})", self.opnd(&d.a, ty)),
                format!("(({t}){})", self.opnd(&d.b, ty)),
                format!("(({t}){})", self.opnd(&d.r, ty)),
                self.opnd(&d.cin, Ty::I32),
                self.opnd(&d.cnt, Ty::I32),
            );
            let v = V { a: &a, b: &b, r: &r, ci: &ci, cn: &cn };
            let mut anyuse = false;
            for k in 0..6 {
                if used & (1 << k) != 0 && d.mask & (1 << k) != 0 {
                    if let Some(e) = bit_formula(d.kind, ty, k, &v) {
                        stmts.push(format!("fc{cid}_{} = {e};", ["cf", "pf", "af", "zf", "sf", "of"][k]));
                        anyuse = true;
                    }
                }
            }
            let _ = anyuse;
        }
        if stmts.is_empty() {
            return;
        }
        if let Some(c) = cnt {
            let s = format!("if ({c} != 0) {{ {} }}", stmts.join(" "));
            self.line(&s);
        } else {
            for s in stmts {
                self.line(&s);
            }
        }
    }

    // ------------------------------------------------------------------ ops
    pub fn trap_stmt(&mut self, addr: u32) -> String {
        let r = format!("guest_unresolved({:#x}u, esp, eax, ecx, edx)", addr);
        self.n_traps.push((addr, 0));
        self.wrap_ret(&r)
    }

    /// `return <GuestRet expr>` in the current function's return type
    pub fn wrap_ret(&self, r: &str) -> String {
        if self.me.ext_in == 0 && self.me.ext_out == 0 {
            format!("return {r};")
        } else {
            let mut s = format!("{{ LxExt e_; e_.r = {r}; ");
            for (i, g) in EXT_ORDER.iter().enumerate() {
                if self.me.ext_out & ext_bit(*g) != 0 && self.gpr_in_scope(*g) {
                    s.push_str(&format!("e_.x[{i}] = {}; ", g.name()));
                } else {
                    s.push_str(&format!("e_.x[{i}] = 0; "));
                }
            }
            if self.me.ext_out & 16 != 0 {
                s.push_str("e_.x[4] = ecx; ");
            } else {
                s.push_str("e_.x[4] = 0; ");
            }
            s.push_str("return e_; }");
            s
        }
    }
    fn gpr_in_scope(&self, _g: Gpr) -> bool {
        true
    }

    pub fn emit_op(&mut self, key: (u32, u32), ins: &Inst) {
        let op = &ins.op;
        match op {
            Op::Mov { ty, d, a } => {
                let r = self.opnd(a, *ty);
                self.assign(d, *ty, &r);
            }
            Op::Bin { op: bop, ty, d, a, b } => {
                let (x, y) = (self.opnd(a, *ty), self.opnd(b, *ty));
                let t = cty(*ty);
                let bits = ty.bits();
                let u32c = |s: &str| format!("(uint32_t){s}");
                let r = match (bop, ty) {
                    (BinOp::Add, _) => format!("({t})({x} + {y})"),
                    (BinOp::Sub, _) => format!("({t})({x} - {y})"),
                    (BinOp::And, _) => format!("({t})({x} & {y})"),
                    (BinOp::Or, _) => format!("({t})({x} | {y})"),
                    (BinOp::Xor, _) => format!("({t})({x} ^ {y})"),
                    (BinOp::Mul, Ty::I64) => format!("({x} * {y})"),
                    (BinOp::Mul, _) => format!("({t})({} * {})", u32c(&x), u32c(&y)),
                    (BinOp::Shl, Ty::I64) => format!("({x} << {y})"),
                    (BinOp::Shl, _) => format!("({t})({} << {y})", u32c(&x)),
                    (BinOp::Shr, Ty::I64) => format!("({x} >> {y})"),
                    (BinOp::Shr, _) => format!("({t})({} >> {y})", u32c(&x)),
                    (BinOp::Sar, Ty::I64) => format!("(uint64_t)((int64_t){x} >> {y})"),
                    (BinOp::Sar, _) => format!("({t})((int32_t)({}){x} >> {y})", sty(*ty)),
                    (BinOp::Rol, _) => match bits {
                        8 => format!("__builtin_rotateleft8({x}, {y})"),
                        16 => format!("__builtin_rotateleft16({x}, {y})"),
                        32 => format!("__builtin_rotateleft32({x}, {y})"),
                        _ => format!("__builtin_rotateleft64({x}, {y})"),
                    },
                    (BinOp::Ror, _) => match bits {
                        8 => format!("__builtin_rotateright8({x}, {y})"),
                        16 => format!("__builtin_rotateright16({x}, {y})"),
                        32 => format!("__builtin_rotateright32({x}, {y})"),
                        _ => format!("__builtin_rotateright64({x}, {y})"),
                    },
                    (BinOp::UDiv, _) => format!("({t})({x} / {y})"),
                    (BinOp::URem, _) => format!("({t})({x} % {y})"),
                    (BinOp::SDiv, _) => format!("({t})(({}){x} / ({}){y})", sty(*ty), sty(*ty)),
                    (BinOp::SRem, _) => format!("({t})(({}){x} % ({}){y})", sty(*ty), sty(*ty)),
                    (BinOp::MulHiU, Ty::I32) => format!("(uint32_t)(((uint64_t){x} * (uint64_t){y}) >> 32)"),
                    (BinOp::MulHiS, Ty::I32) => format!("(uint32_t)((int64_t)(int32_t){x} * (int64_t)(int32_t){y} >> 32)"),
                    (BinOp::MulHiU, _) | (BinOp::MulHiS, _) => "0".into(),
                };
                self.assign(d, *ty, &r);
            }
            Op::Un { op: uop, ty, d, a } => {
                let x = self.opnd(a, *ty);
                let t = cty(*ty);
                let r = match uop {
                    UnOp::Not => format!("({t})(~{x})"),
                    UnOp::Neg => format!("({t})(0u - {x})"),
                    UnOp::Bswap => format!("__builtin_bswap32({x})"),
                    UnOp::Clz => format!("((uint32_t)({x} ? __builtin_clz({x}) : 32))"),
                    UnOp::Ctz => format!("((uint32_t)({x} ? __builtin_ctz({x}) : 32))"),
                };
                self.assign(d, *ty, &r);
            }
            Op::Cast { k, from, to, d, a } => {
                let x = self.opnd(a, *from);
                let t = cty(*to);
                let r = match k {
                    CastKind::Zx | CastKind::Trunc => format!("({t}){x}"),
                    CastKind::Sx => format!("({t})({}){x}", sty(*from)),
                };
                // sign extension to 64: go through the signed type of the source
                let r = if *k == CastKind::Sx && *to == Ty::I64 { format!("(uint64_t)(int64_t)({}){x}", sty(*from)) } else { r };
                self.assign(d, *to, &r);
            }
            Op::Sel { ty, d, c, a, b } => {
                let (cc, x, y) = (self.opnd(c, Ty::I1), self.opnd(a, *ty), self.opnd(b, *ty));
                self.assign(d, *ty, &format!("({cc} ? {x} : {y})"));
            }
            Op::Cmp { pred, ty, d, a, b } => {
                let (x, y) = (self.opnd(a, *ty), self.opnd(b, *ty));
                let s = sty(*ty);
                let r = match pred {
                    Pred::Eq => format!("({x} == {y})"),
                    Pred::Ne => format!("({x} != {y})"),
                    Pred::Ult => format!("({x} < {y})"),
                    Pred::Ule => format!("({x} <= {y})"),
                    Pred::Ugt => format!("({x} > {y})"),
                    Pred::Uge => format!("({x} >= {y})"),
                    Pred::Slt => format!("(({s}){x} < ({s}){y})"),
                    Pred::Sle => format!("(({s}){x} <= ({s}){y})"),
                    Pred::Sgt => format!("(({s}){x} > ({s}){y})"),
                    Pred::Sge => format!("(({s}){x} >= ({s}){y})"),
                };
                self.assign(d, Ty::I1, &r);
            }
            Op::Lea { d, m } => {
                let r = self.addr(m);
                self.assign(d, Ty::I32, &r);
            }
            Op::Ld { ty, d, m } => {
                let r = self.ld_int(ty.bytes(), m);
                let r = if *ty == Ty::I1 { r } else { r };
                self.assign(d, *ty, &r);
            }
            Op::St { ty, m, v } => {
                let x = self.opnd(v, *ty);
                let s = self.st_int(ty.bytes(), m, &x);
                self.line(&s);
            }
            Op::SetPart { r, ty, shift, v } => {
                let x = self.opnd(v, *ty);
                let n = r.name();
                let mask = (ty.mask() << shift) as u32;
                let s = if *shift == 0 {
                    format!("{n} = ({n} & {:#x}u) | (uint32_t){x};", !mask)
                } else {
                    format!("{n} = ({n} & {:#x}u) | ((uint32_t){x} << {shift}u);", !mask)
                };
                self.line(&s);
            }
            Op::MulWide { signed, ty, hi, lo, a, b } => {
                let (x, y) = (self.opnd(a, *ty), self.opnd(b, *ty));
                let (wt, st) = match ty {
                    Ty::I8 => ("uint16_t", "int16_t"),
                    Ty::I16 => ("uint32_t", "int32_t"),
                    _ => ("uint64_t", "int64_t"),
                };
                let sty_ = sty(*ty);
                let p = if *signed { format!("({wt})(({st})({sty_}){x} * ({st})({sty_}){y})") } else { format!("({wt})(({wt}){x} * ({wt}){y})") };
                let bits = ty.bits();
                let t = cty(*ty);
                let pt = format!("p{}", self.cur_blk);
                let s = format!("{{ {wt} {pt} = {p};");
                self.line(&s);
                self.assign_inline(lo, &format!("({t}){pt}"));
                self.assign_inline(hi, &format!("({t})({pt} >> {bits}u)"));
                self.line("}");
            }
            Op::DivWide { signed, ty, q, r, hi, lo, div } => {
                let (h, l, dv) = (self.opnd(hi, *ty), self.opnd(lo, *ty), self.opnd(div, *ty));
                let t = cty(*ty);
                let bits = ty.bits();
                let (wt, st) = match ty {
                    Ty::I8 => ("uint16_t", "int16_t"),
                    Ty::I16 => ("uint32_t", "int32_t"),
                    _ => ("uint64_t", "int64_t"),
                };
                let sty_ = sty(*ty);
                let trap = self.trap_stmt(ins.addr);
                let (qs, rs) = (format!("dq{}", self.cur_blk), format!("dr{}", self.cur_blk));
                if *signed {
                    let s = format!("{{ {st} dn = ({st})(({wt})((({wt}){h} << {bits}u) | ({wt}){l})); {st} dd = ({st})({sty_}){dv}; if (dd == 0 || (dd == -1 && dn == ({st})(({wt})1 << {})) ) {{ {trap} }} {st} {qs} = dn / dd; if ({qs} != ({st})({sty_}){qs}) {{ {trap} }} {st} {rs} = dn % dd;", 2 * bits - 1);
                    self.line(&s);
                } else {
                    let s = format!("{{ {wt} dn = (({wt}){h} << {bits}u) | ({wt}){l}; {wt} dd = ({wt}){dv}; if (dd == 0) {{ {trap} }} {wt} {qs} = dn / dd; if ({qs} >> {bits}u) {{ {trap} }} {wt} {rs} = dn % dd;");
                    self.line(&s);
                }
                self.assign_inline(q, &format!("({t}){qs}"));
                self.assign_inline(r, &format!("({t}){rs}"));
                self.line("}");
            }
            Op::AtomicRmw { op: aop, ty, old, m, v } => {
                let x = self.opnd(v, *ty);
                let t = cty(*ty);
                let a = self.addr(m);
                let f = match aop {
                    AtomicOp::Add => "__atomic_fetch_add",
                    AtomicOp::Sub => "__atomic_fetch_sub",
                    AtomicOp::And => "__atomic_fetch_and",
                    AtomicOp::Or => "__atomic_fetch_or",
                    AtomicOp::Xor => "__atomic_fetch_xor",
                    AtomicOp::Xchg => "__atomic_exchange_n",
                };
                self.assign(old, *ty, &format!("{f}(({t} *)G({a}), ({t}){x}, __ATOMIC_SEQ_CST)"));
            }
            Op::AtomicCas { ty, old, m, expected, new } => {
                let (e, n) = (self.opnd(expected, *ty), self.opnd(new, *ty));
                let t = cty(*ty);
                let a = self.addr(m);
                let tn = format!("cas{}", self.cur_blk);
                self.line(&format!("{{ {t} {tn} = ({t}){e}; __atomic_compare_exchange_n(({t} *)G({a}), &{tn}, ({t}){n}, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);"));
                self.assign_inline(old, &tn);
                self.line("}");
            }
            Op::Fence => self.line("__atomic_thread_fence(__ATOMIC_SEQ_CST);"),
            Op::FlagDef(d) => self.emit_flagdef(key, d),
            Op::FlagGet { d, .. } => {
                let rhs = self.cons_rhs.get(&key).cloned().unwrap_or_else(|| "0".into());
                self.assign(d, Ty::I1, &rhs);
            }
            Op::FlagGetBit { d, .. } | Op::FlagsPack { d } => {
                let rhs = self.cons_rhs.get(&key).cloned().unwrap_or_else(|| "0".into());
                self.assign(d, Ty::I32, &rhs);
            }
            Op::SetDf(v) => {
                if !matches!(v, Opnd::Imm(0)) {
                    let x = self.opnd(v, Ty::I32);
                    self.line(&format!("df = {x};"));
                }
            }
            Op::GetDf { d } => {
                self.assign(d, Ty::I32, "df");
            }
            Op::Call { target, ret_addr } => self.emit_call(target, *ret_addr),
            Op::Str { .. } => self.emit_str(op),
            Op::Misc(m) => self.emit_misc(m),
            Op::ResumeSetup { d, conts } => {
                // _setjmp must be called in this frame; a resume (runtime guest_resume) longjmps back here with resume_target set
                self.uses_pad = true;
                let dn = self.place(d);
                let mut s = format!("if (__builtin_expect(lx_setjmp(pad_.jb), 0)) {{ ebp = gt->resume_ebp; switch (gt->resume_target) {{");
                for (i, c) in conts.iter().enumerate() {
                    let _ = write!(s, " case {c:#x}u: {dn} = {}u; break;", i + 1);
                }
                let _ = write!(s, " default: {dn} = {}u; break; }} }} else {{ guest_pad_push(&pad_, ebp); {dn} = 0u; }}", conts.len() + 1);
                self.line(&s);
            }
            Op::Trap { why } => {
                let s = self.trap_stmt(ins.addr);
                let _ = why;
                self.line(&s);
            }
            _ => {
                if !self.emit_fp_op(key, ins) {
                    let s = format!("/* unhandled op {:?} */", std::mem::discriminant(op));
                    self.line(&s);
                    let t = self.trap_stmt(ins.addr);
                    self.line(&t);
                }
            }
        }
    }

}
