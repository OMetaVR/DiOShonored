//! x87 and SSE operation emission.
use crate::emit::*;
use ir::*;

impl<'a> Emitter<'a> {
    pub fn emit_fp_decls(&mut self, h: &mut String) {
        let mut maxf: i32 = -1;
        let mut any = false;
        let mut xmm = [false; 16];
        for b in &self.f.blocks {
            for i in &b.insts {
                if i.op.is_x87() {
                    any = true;
                }
                let mut f = |p: Place| match p {
                    Place::Fp(k) => maxf = maxf.max(k as i32),
                    Place::Xmm(k) => xmm[k as usize] = true,
                    Place::Mm(k) => xmm[8 + k as usize] = true,
                    _ => {}
                };
                i.op.visit_uses(&mut f);
                i.op.visit_defs(&mut f);
            }
        }
        if maxf >= 0 {
            let v: Vec<String> = (0..=maxf).map(|k| format!("f{k} = 0")).collect();
            h.push_str(&format!("    double {};\n", v.join(", ")));
        }
        if any {
            h.push_str("    uint16_t fcc = 0;\n");
        }
        let v: Vec<String> = (0..16).filter(|k| xmm[*k]).map(|k| format!("xmm{k} = {{0}}")).collect();
        if !v.is_empty() {
            h.push_str(&format!("    v128 {};\n", v.join(", ")));
        }
    }

    fn fp_ld(&self, fmt: FMem, m: &Mem) -> String {
        match fmt {
            FMem::F32 => format!("(double)lx_f32_from_bits({})", self.ld_int(4, m)),
            FMem::F64 => format!("lx_f64_from_bits({})", self.ld_int(8, m)),
            FMem::F80 => format!("lx_ld80(G({}))", self.addr(m)),
            FMem::I16 => format!("(double)(int16_t){}", self.ld_int(2, m)),
            FMem::I32 => format!("(double)(int32_t){}", self.ld_int(4, m)),
            FMem::I64 => format!("(double)(int64_t){}", self.ld_int(8, m)),
        }
    }

    pub fn emit_fp_op(&mut self, key: (u32, u32), ins: &Inst) -> bool {
        let _ = key;
        let op = &ins.op;
        let fo = |s: &Self, o: &Opnd| s.opnd(o, Ty::F64);
        match op {
            Op::FMov { d, a } => {
                let r = fo(self, a);
                self.assign(d, Ty::F64, &r);
            }
            Op::FConst { d, v } => {
                let r = format!("lx_f64_from_bits({:#x}ull)", v.to_bits());
                self.assign(d, Ty::F64, &r);
            }
            Op::FBin { op: bop, d, a, b } => {
                let (x, y) = (fo(self, a), fo(self, b));
                let c = match bop {
                    FOp::Add => '+',
                    FOp::Sub => '-',
                    FOp::Mul => '*',
                    FOp::Div => '/',
                };
                self.assign(d, Ty::F64, &format!("{x} {c} {y}"));
            }
            Op::FUn { op: u, d, a } => {
                let x = fo(self, a);
                let r = match u {
                    FUn::Neg => format!("-{x}"),
                    FUn::Abs => format!("__builtin_fabs({x})"),
                    FUn::Sqrt => format!("__builtin_sqrt({x})"),
                    FUn::Sin => format!("__builtin_sin({x})"),
                    FUn::Cos => format!("__builtin_cos({x})"),
                    FUn::Tan => format!("__builtin_tan({x})"),
                    FUn::Rndint => format!("lx_rndint({x}, gt->x87_cw)"),
                    FUn::F2xm1 => format!("__builtin_expm1({x} * 0.6931471805599453)"),
                };
                self.assign(d, Ty::F64, &r);
            }
            Op::FLdM { fmt, d, m } => {
                let r = self.fp_ld(*fmt, m);
                self.assign(d, Ty::F64, &r);
            }
            Op::FStM { fmt, m, a, trunc } => {
                let x = fo(self, a);
                let s = match fmt {
                    FMem::F32 => self.st_int(4, m, &format!("lx_f32_to_bits((float){x})")),
                    FMem::F64 => self.st_int(8, m, &format!("lx_f64_to_bits({x})")),
                    FMem::F80 => format!("lx_st80(G({}), {x});", self.addr(m)),
                    FMem::I16 => self.st_int(2, m, &format!("lx_fist16({x}, {}, {})", if *trunc { "0xC00u".to_string() } else { "gt->x87_cw".to_string() }, 0)),
                    FMem::I32 => self.st_int(4, m, &format!("lx_fist32({x}, {})", if *trunc { "0xC00u".to_string() } else { "gt->x87_cw".to_string() })),
                    FMem::I64 => self.st_int(8, m, &format!("lx_fist64({x}, {})", if *trunc { "0xC00u".to_string() } else { "gt->x87_cw".to_string() })),
                };
                self.line(&s);
            }
            Op::FCom { a, b } => {
                let (x, y) = (fo(self, a), fo(self, b));
                self.line(&format!("fcc = ({x} > {y}) ? 0u : ({x} < {y}) ? 0x100u : ({x} == {y}) ? 0x4000u : 0x4500u;"));
            }
            Op::FTst { a } => {
                let x = fo(self, a);
                self.line(&format!("fcc = ({x} > 0.0) ? 0u : ({x} < 0.0) ? 0x100u : ({x} == 0.0) ? 0x4000u : 0x4500u;"));
            }
            Op::FComi { d, a, b } => {
                let (x, y) = (fo(self, a), fo(self, b));
                self.assign(d, Ty::I32, &format!("({x} > {y}) ? 0u : ({x} < {y}) ? 1u : ({x} == {y}) ? 0x40u : 0x45u"));
            }
            Op::FXam { a } => {
                let x = fo(self, a);
                self.line(&format!("fcc = lx_fxam({x});"));
            }
            Op::FNstsw { d, top } => {
                let dn = self.place(d);
                self.line(&format!("{dn} = (uint16_t)(fcc | {}u);", (*top as u32) << 11));
            }
            Op::FNstcw { d } => {
                let dn = self.place(d);
                self.line(&format!("{dn} = (uint16_t)gt->x87_cw;"));
            }
            Op::FLdcw { v } => {
                let x = self.opnd(v, Ty::I16);
                self.line(&format!("gt->x87_cw = {x};"));
            }
            Op::FCmov { c, d, a } => {
                let (cc, x) = (self.opnd(c, Ty::I1), fo(self, a));
                let dn = self.place(d);
                self.line(&format!("if ({cc}) {dn} = {x};"));
            }
            Op::FPrem { ieee, d, b } => {
                let y = fo(self, b);
                let dn = self.place(d);
                self.line(&format!("{dn} = lx_fprem({dn}, {y}, {}, &fcc);", if *ieee { 1 } else { 0 }));
            }
            Op::FScale { d, b } => {
                let y = fo(self, b);
                let dn = self.place(d);
                self.line(&format!("{dn} = lx_fscale({dn}, {y});"));
            }
            Op::FYl2x { d, a, b } => {
                let (x, y) = (fo(self, a), fo(self, b));
                self.assign(d, Ty::F64, &format!("{x} * __builtin_log2({y})"));
            }
            Op::FAtan2 { d, y, x } => {
                let (yy, xx) = (fo(self, y), fo(self, x));
                self.assign(d, Ty::F64, &format!("__builtin_atan2({yy}, {xx})"));
            }
            Op::FRetLoad { d } => {
                self.assign(d, Ty::F64, "gt->st0_ret");
            }
            Op::FRetStore { a } => {
                let x = fo(self, a);
                self.line(&format!("gt->st0_ret = {x};"));
            }
            Op::FHost { sym, n, d, a, b } => {
                let name = self.callees.sym_name(*sym);
                self.x87_host_refs.insert(name.clone());
                let (x, y) = (fo(self, a), fo(self, b));
                if *n == 2 {
                    self.assign(d, Ty::F64, &format!("{name}({y}, {x})"));
                } else {
                    self.assign(d, Ty::F64, &format!("{name}({x})"));
                }
            }
            Op::FInit => {
                self.line("gt->x87_cw = 0x37Fu; fcc = 0;");
            }
            _ => return self.emit_sse_op(ins),
        }
        true
    }

    pub fn emit_sse_op(&mut self, ins: &Inst) -> bool {
        match &ins.op {
            Op::V { f, ty, d, a, b, imm } => {
                let name = f.0;
                let mut args: Vec<String> = Vec::new();
                if !matches!(a, Opnd::Imm(_)) {
                    args.push(self.opnd(a, Ty::V128));
                }
                let omit_b = matches!(b, Opnd::Imm(0)) && (name == "psrldq_i" || name == "pslldq_i");
                if !omit_b {
                    args.push(self.opnd(b, Ty::I32));
                }
                if name.ends_with("_i") {
                    args.push(format!("{}", imm));
                }
                let r = format!("lx_{}({})", name, args.join(", "));
                self.assign(d, *ty, &r);
            }
            Op::VLd { w, zx, d, m } => {
                let r = match (*w, *zx) {
                    (16, _) => format!("lx_ld128(G({}))", self.addr(m)),
                    (4, true) => format!("lx_vfrom32({})", self.ld_int(4, m)),
                    (8, true) => format!("lx_vfrom64({})", self.ld_int(8, m)),
                    (4, false) => format!("lx_vmerge32({}, {})", self.place(d), self.ld_int(4, m)),
                    _ => format!("lx_vmerge64({}, {})", self.place(d), self.ld_int(8, m)),
                };
                self.assign(d, Ty::V128, &r);
            }
            Op::VSt { w, m, a } => {
                let x = self.opnd(a, Ty::V128);
                let s = match *w {
                    16 => format!("{{ v128 s_ = {x}; __builtin_memcpy(G({}), &s_, 16); }}", self.addr(m)),
                    4 => self.st_int(4, m, &format!("lx_vto32({x})")),
                    _ => self.st_int(8, m, &format!("lx_vto64({x})")),
                };
                self.line(&s);
            }
            _ => return false,
        }
        true
    }
}
