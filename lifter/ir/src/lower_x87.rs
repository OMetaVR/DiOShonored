//! x87 lowering: operations on the relative register stack (`Place::St(i)`); `FPush`/`FPop`/`FXch` express depth changes.
//! The depth is resolved statically by `lift::x87` afterwards. Values are `double` (53-bit precision, round-to-nearest).

use crate::lower::*;
use crate::repr::*;
use iced_x86::{Instruction, MemorySize, Mnemonic, OpKind, Register};

fn st_index(r: Register) -> Option<u8> {
    use Register::*;
    Some(match r {
        ST0 => 0,
        ST1 => 1,
        ST2 => 2,
        ST3 => 3,
        ST4 => 4,
        ST5 => 5,
        ST6 => 6,
        ST7 => 7,
        _ => return Option::None,
    })
}

impl<'a> Lower<'a> {
    fn fmem_of(&self, i: &Instruction) -> Option<FMem> {
        Some(match i.memory_size() {
            MemorySize::Float32 => FMem::F32,
            MemorySize::Float64 => FMem::F64,
            MemorySize::Float80 => FMem::F80,
            MemorySize::Int16 | MemorySize::UInt16 => FMem::I16,
            MemorySize::Int32 | MemorySize::UInt32 => FMem::I32,
            MemorySize::Int64 | MemorySize::UInt64 => FMem::I64,
            _ => return Option::None,
        })
    }

    fn fld_mem(&mut self, i: &Instruction) -> Option<Opnd> {
        let fmt = self.fmem_of(i)?;
        let m = self.mem(i);
        let d = self.tmp(Ty::F64);
        self.emit(Op::FLdM { fmt, d, m });
        Some(Opnd::P(d))
    }

    fn fpush_const(&mut self, v: f64) {
        let d = self.tmp(Ty::F64);
        self.emit(Op::FConst { d, v });
        self.emit(Op::FPush { v: Opnd::P(d) });
    }

    fn farith(&mut self, i: &Instruction, op: FOp, rev: bool, pop: bool) -> bool {
        let (d, a, b);
        if i.op0_kind() == OpKind::Memory {
            let src = match self.fld_mem(i) {
                Some(s) => s,
                None => return false,
            };
            let st0 = Opnd::P(Place::St(0));
            d = Place::St(0);
            if rev {
                a = src;
                b = st0;
            } else {
                a = st0;
                b = src;
            }
        } else {
            let dr = match st_index(i.op0_register()) {
                Some(x) => x,
                None => return false,
            };
            let sr = if i.op_count() > 1 { st_index(i.op1_register()) } else { Some(0) };
            let sr = match sr {
                Some(x) => x,
                None => return false,
            };
            d = Place::St(dr);
            let (dd, ss) = (Opnd::P(Place::St(dr)), Opnd::P(Place::St(sr)));
            if rev {
                a = ss;
                b = dd;
            } else {
                a = dd;
                b = ss;
            }
        }
        self.emit(Op::FBin { op, d, a, b });
        if pop {
            self.emit(Op::FPop);
        }
        true
    }

    fn fcmp(&mut self, i: &Instruction, pops: u8, int_mem: bool) -> bool {
        let _ = int_mem;
        let other = if i.op_count() == 0 {
            Opnd::P(Place::St(1))
        } else if i.op0_kind() == OpKind::Memory {
            match self.fld_mem(i) {
                Some(s) => s,
                None => return false,
            }
        } else {
            match st_index(i.op0_register()) {
                Some(0) if i.op_count() > 1 => match st_index(i.op1_register()) {
                    Some(x) => Opnd::P(Place::St(x)),
                    None => return false,
                },
                Some(x) => Opnd::P(Place::St(x)),
                None => return false,
            }
        };
        self.emit(Op::FCom { a: Opnd::P(Place::St(0)), b: other });
        for _ in 0..pops {
            self.emit(Op::FPop);
        }
        true
    }

    fn fcomi(&mut self, i: &Instruction, pop: bool) -> bool {
        let other = match st_index(i.op1_register()) {
            Some(x) => Opnd::P(Place::St(x)),
            None => return false,
        };
        let d = self.tmp(Ty::I32);
        self.emit(Op::FComi { d, a: Opnd::P(Place::St(0)), b: other });
        self.emit(Op::FlagDef(FlagDef { kind: FlagKind::Raw, ty: Ty::I32, mask: FL_ALL, a: Opnd::Imm(0), b: Opnd::Imm(0), r: Opnd::P(d), cin: Opnd::Imm(0), cnt: Opnd::Imm(0), cond_cnt: false }));
        if pop {
            self.emit(Op::FPop);
        }
        true
    }

    /// returns true if the instruction was handled (possibly marking it unsupported)
    pub fn lower_x87(&mut self, i: &Instruction) -> bool {
        use Mnemonic as M;
        let m = i.mnemonic();
        let st0 = Opnd::P(Place::St(0));
        match m {
            M::Fld => {
                if i.op0_kind() == OpKind::Memory {
                    match self.fld_mem(i) {
                        Some(v) => self.emit(Op::FPush { v }),
                        None => self.unsupported("fld mem size"),
                    }
                } else if let Some(x) = st_index(i.op0_register()) {
                    self.emit(Op::FPush { v: Opnd::P(Place::St(x)) });
                } else {
                    self.unsupported("fld operand");
                }
            }
            M::Fild => match self.fld_mem(i) {
                Some(v) => self.emit(Op::FPush { v }),
                None => self.unsupported("fild size"),
            },
            M::Fld1 => self.fpush_const(1.0),
            M::Fldz => self.fpush_const(0.0),
            M::Fldpi => self.fpush_const(std::f64::consts::PI),
            M::Fldl2e => self.fpush_const(std::f64::consts::LOG2_E),
            M::Fldl2t => self.fpush_const(std::f64::consts::LOG2_10),
            M::Fldlg2 => self.fpush_const(std::f64::consts::LOG10_2),
            M::Fldln2 => self.fpush_const(std::f64::consts::LN_2),
            M::Fst | M::Fstp => {
                let pop = m == M::Fstp;
                if i.op0_kind() == OpKind::Memory {
                    let fmt = match self.fmem_of(i) {
                        Some(f) => f,
                        None => {
                            self.unsupported("fst size");
                            return true;
                        }
                    };
                    let mm = self.mem(i);
                    self.emit(Op::FStM { fmt, m: mm, a: st0, trunc: false });
                } else if let Some(x) = st_index(i.op0_register()) {
                    if x != 0 {
                        self.emit(Op::FMov { d: Place::St(x), a: st0 });
                    }
                } else {
                    self.unsupported("fst operand");
                }
                if pop {
                    self.emit(Op::FPop);
                }
            }
            M::Fist | M::Fistp | M::Fisttp => {
                let fmt = match self.fmem_of(i) {
                    Some(f) => f,
                    None => {
                        self.unsupported("fist size");
                        return true;
                    }
                };
                let mm = self.mem(i);
                self.emit(Op::FStM { fmt, m: mm, a: st0, trunc: m == M::Fisttp });
                if m != M::Fist {
                    self.emit(Op::FPop);
                }
            }
            M::Fxch => {
                let x = match i.op_count() {
                    0 => 1,
                    1 => st_index(i.op0_register()).unwrap_or(1),
                    _ => st_index(i.op1_register()).unwrap_or(1),
                };
                if x != 0 {
                    self.emit(Op::FXch { i: x });
                }
            }
            M::Fadd | M::Faddp | M::Fiadd => {
                if !self.farith(i, FOp::Add, false, m == M::Faddp) {
                    self.unsupported("fadd form");
                }
            }
            M::Fmul | M::Fmulp | M::Fimul => {
                if !self.farith(i, FOp::Mul, false, m == M::Fmulp) {
                    self.unsupported("fmul form");
                }
            }
            M::Fsub | M::Fsubp | M::Fisub => {
                if !self.farith(i, FOp::Sub, false, m == M::Fsubp) {
                    self.unsupported("fsub form");
                }
            }
            M::Fsubr | M::Fsubrp | M::Fisubr => {
                if !self.farith(i, FOp::Sub, true, m == M::Fsubrp) {
                    self.unsupported("fsubr form");
                }
            }
            M::Fdiv | M::Fdivp | M::Fidiv => {
                if !self.farith(i, FOp::Div, false, m == M::Fdivp) {
                    self.unsupported("fdiv form");
                }
            }
            M::Fdivr | M::Fdivrp | M::Fidivr => {
                if !self.farith(i, FOp::Div, true, m == M::Fdivrp) {
                    self.unsupported("fdivr form");
                }
            }
            M::Fchs => self.emit(Op::FUn { op: FUn::Neg, d: Place::St(0), a: st0 }),
            M::Fabs => self.emit(Op::FUn { op: FUn::Abs, d: Place::St(0), a: st0 }),
            M::Fsqrt => self.emit(Op::FUn { op: FUn::Sqrt, d: Place::St(0), a: st0 }),
            M::Fsin => self.emit(Op::FUn { op: FUn::Sin, d: Place::St(0), a: st0 }),
            M::Fcos => self.emit(Op::FUn { op: FUn::Cos, d: Place::St(0), a: st0 }),
            M::Frndint => self.emit(Op::FUn { op: FUn::Rndint, d: Place::St(0), a: st0 }),
            M::F2xm1 => self.emit(Op::FUn { op: FUn::F2xm1, d: Place::St(0), a: st0 }),
            M::Fptan => {
                self.emit(Op::FUn { op: FUn::Tan, d: Place::St(0), a: st0 });
                self.fpush_const(1.0);
            }
            M::Fsincos => {
                let s = self.tmp(Ty::F64);
                let c = self.tmp(Ty::F64);
                self.emit(Op::FUn { op: FUn::Sin, d: s, a: st0 });
                self.emit(Op::FUn { op: FUn::Cos, d: c, a: st0 });
                self.emit(Op::FMov { d: Place::St(0), a: Opnd::P(s) });
                self.emit(Op::FPush { v: Opnd::P(c) });
            }
            M::Fscale => self.emit(Op::FScale { d: Place::St(0), b: Opnd::P(Place::St(1)) }),
            M::Fprem | M::Fprem1 => self.emit(Op::FPrem { ieee: m == M::Fprem1, d: Place::St(0), b: Opnd::P(Place::St(1)) }),
            M::Fyl2x => {
                self.emit(Op::FYl2x { d: Place::St(1), a: Opnd::P(Place::St(1)), b: st0 });
                self.emit(Op::FPop);
            }
            M::Fpatan => {
                self.emit(Op::FAtan2 { d: Place::St(1), y: Opnd::P(Place::St(1)), x: st0 });
                self.emit(Op::FPop);
            }
            M::Fcom | M::Fucom => {
                if !self.fcmp(i, 0, false) {
                    self.unsupported("fcom form");
                }
            }
            M::Fcomp | M::Fucomp => {
                if !self.fcmp(i, 1, false) {
                    self.unsupported("fcomp form");
                }
            }
            M::Fcompp | M::Fucompp => {
                self.emit(Op::FCom { a: st0, b: Opnd::P(Place::St(1)) });
                self.emit(Op::FPop);
                self.emit(Op::FPop);
            }
            M::Ficom | M::Ficomp => {
                if !self.fcmp(i, if m == M::Ficomp { 1 } else { 0 }, true) {
                    self.unsupported("ficom form");
                }
            }
            M::Fcomi | M::Fucomi => {
                if !self.fcomi(i, false) {
                    self.unsupported("fcomi form");
                }
            }
            M::Fcomip | M::Fucomip => {
                if !self.fcomi(i, true) {
                    self.unsupported("fcomip form");
                }
            }
            M::Ftst => self.emit(Op::FTst { a: st0 }),
            M::Fxam => self.emit(Op::FXam { a: st0 }),
            M::Fnstsw | M::Fstsw => {
                let t = self.tmp(Ty::I16);
                self.emit(Op::FNstsw { d: t, top: 0 });
                if i.op0_kind() == OpKind::Register {
                    self.write_reg(Register::AX, Opnd::P(t));
                } else {
                    let mm = self.mem(i);
                    self.emit(Op::St { ty: Ty::I16, m: mm, v: Opnd::P(t) });
                }
            }
            M::Fnstcw | M::Fstcw => {
                let t = self.tmp(Ty::I16);
                self.emit(Op::FNstcw { d: t });
                let mm = self.mem(i);
                self.emit(Op::St { ty: Ty::I16, m: mm, v: Opnd::P(t) });
            }
            M::Fldcw => {
                let v = self.read_op(i, 0, Ty::I16);
                self.emit(Op::FLdcw { v });
            }
            M::Fnclex | M::Fclex => {}
            M::Fninit | M::Finit => self.emit(Op::FInit),
            M::Fcmovb | M::Fcmove | M::Fcmovbe | M::Fcmovu | M::Fcmovnb | M::Fcmovne | M::Fcmovnbe | M::Fcmovnu => {
                let cond = match m {
                    M::Fcmovb => Cond::B,
                    M::Fcmove => Cond::E,
                    M::Fcmovbe => Cond::Be,
                    M::Fcmovu => Cond::P,
                    M::Fcmovnb => Cond::Ae,
                    M::Fcmovne => Cond::Ne,
                    M::Fcmovnbe => Cond::A,
                    _ => Cond::Np,
                };
                let c = self.tmp(Ty::I1);
                self.emit(Op::FlagGet { d: c, cond });
                let x = st_index(i.op1_register()).unwrap_or(1);
                self.emit(Op::FCmov { c: Opnd::P(c), d: Place::St(0), a: Opnd::P(Place::St(x)) });
            }
            M::Ffree | M::Ffreep | M::Fincstp | M::Fdecstp | M::Fxtract | M::Fyl2xp1 | M::Fbld | M::Fbstp | M::Fnsave | M::Fsave | M::Frstor | M::Fldenv | M::Fnstenv | M::Fstenv | M::Fxsave | M::Fxrstor => {
                self.unsupported(&format!("x87 {:?}", m));
            }
            _ => return false,
        }
        true
    }
}
