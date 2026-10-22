//! x86-32 instruction -> IR lowering (integer, control flow, string ops). The semantics of every instruction are defined here
//! and nowhere else. x87 and SSE live in `lower_x87.rs` / `lower_sse.rs`.

use crate::repr::*;
use iced_x86::{Code, ConditionCode, Instruction, Mnemonic, OpKind, Register};

pub const TRAP_UNSUPPORTED: u16 = 1;
pub const TRAP_INT3: u16 = 2;
pub const TRAP_INVALID: u16 = 3;
pub const TRAP_PRIV: u16 = 4;
pub const TRAP_UD2: u16 = 5;
pub const TRAP_DIV: u16 = 6;

/// Facts the lowering needs from the surrounding program.
pub trait LowerEnv {
    /// `call [slot]` / `jmp [slot]` through an IAT slot: resolved target if `slot` is a known IAT slot
    fn iat_target(&self, slot: u32) -> Option<CallTarget>;
    fn is_noreturn(&self, t: &CallTarget) -> bool;
    /// jump table targets for an indirect `jmp [reg*4+tbl]` at `site`
    fn jump_table(&self, site: u32) -> Option<Vec<u32>>;
}

#[derive(Debug, Clone)]
pub enum Ctl {
    Next,
    Jmp(u32),
    Jcc(Opnd, u32),
    Switch(Opnd, Vec<u32>),
    TailInd(CallTarget),
    Ret(u16),
    /// control does not continue (noreturn call)
    Stop,
    Trap(u16),
}

pub struct Lower<'a> {
    pub env: &'a dyn LowerEnv,
    pub tmp_ty: &'a mut Vec<Ty>,
    pub ops: Vec<Op>,
    pub addr: u32,
    pub next: u32,
    pub unsupported: Option<String>,
}

pub(crate) fn gpr_part(r: Register) -> Option<(Gpr, Ty, u8)> {
    use Register::*;
    Some(match r {
        EAX => (Gpr::Eax, Ty::I32, 0),
        ECX => (Gpr::Ecx, Ty::I32, 0),
        EDX => (Gpr::Edx, Ty::I32, 0),
        EBX => (Gpr::Ebx, Ty::I32, 0),
        ESP => (Gpr::Esp, Ty::I32, 0),
        EBP => (Gpr::Ebp, Ty::I32, 0),
        ESI => (Gpr::Esi, Ty::I32, 0),
        EDI => (Gpr::Edi, Ty::I32, 0),
        AX => (Gpr::Eax, Ty::I16, 0),
        CX => (Gpr::Ecx, Ty::I16, 0),
        DX => (Gpr::Edx, Ty::I16, 0),
        BX => (Gpr::Ebx, Ty::I16, 0),
        SP => (Gpr::Esp, Ty::I16, 0),
        BP => (Gpr::Ebp, Ty::I16, 0),
        SI => (Gpr::Esi, Ty::I16, 0),
        DI => (Gpr::Edi, Ty::I16, 0),
        AL => (Gpr::Eax, Ty::I8, 0),
        CL => (Gpr::Ecx, Ty::I8, 0),
        DL => (Gpr::Edx, Ty::I8, 0),
        BL => (Gpr::Ebx, Ty::I8, 0),
        AH => (Gpr::Eax, Ty::I8, 8),
        CH => (Gpr::Ecx, Ty::I8, 8),
        DH => (Gpr::Edx, Ty::I8, 8),
        BH => (Gpr::Ebx, Ty::I8, 8),
        _ => return Option::None,
    })
}

pub(crate) fn cond_of(c: ConditionCode) -> Option<Cond> {
    use ConditionCode as C;
    Some(match c {
        C::o => Cond::O,
        C::no => Cond::No,
        C::b => Cond::B,
        C::ae => Cond::Ae,
        C::e => Cond::E,
        C::ne => Cond::Ne,
        C::be => Cond::Be,
        C::a => Cond::A,
        C::s => Cond::S,
        C::ns => Cond::Ns,
        C::p => Cond::P,
        C::np => Cond::Np,
        C::l => Cond::L,
        C::ge => Cond::Ge,
        C::le => Cond::Le,
        C::g => Cond::G,
        C::None => return None,
    })
}

impl<'a> Lower<'a> {
    pub fn new(env: &'a dyn LowerEnv, tmp_ty: &'a mut Vec<Ty>) -> Lower<'a> {
        Lower { env, tmp_ty, ops: Vec::new(), addr: 0, next: 0, unsupported: None }
    }

    // ------------------------------------------------------------------------------------------ helpers
    pub fn tmp(&mut self, ty: Ty) -> Place {
        self.tmp_ty.push(ty);
        Place::Tmp((self.tmp_ty.len() - 1) as Tmp)
    }
    pub fn emit(&mut self, op: Op) {
        self.ops.push(op);
    }
    pub fn unsupported(&mut self, why: &str) {
        if self.unsupported.is_none() {
            self.unsupported = Some(why.to_string());
        }
    }
    pub fn gpr(&self, g: Gpr) -> Opnd {
        Opnd::P(Place::Gpr(g))
    }
    fn imm(&self, v: u64) -> Opnd {
        Opnd::Imm(v)
    }

    fn bin(&mut self, op: BinOp, ty: Ty, a: Opnd, b: Opnd) -> Opnd {
        let d = self.tmp(ty);
        self.emit(Op::Bin { op, ty, d, a, b });
        Opnd::P(d)
    }
    fn un(&mut self, op: UnOp, ty: Ty, a: Opnd) -> Opnd {
        let d = self.tmp(ty);
        self.emit(Op::Un { op, ty, d, a });
        Opnd::P(d)
    }
    pub fn cast(&mut self, k: CastKind, from: Ty, to: Ty, a: Opnd) -> Opnd {
        if from == to {
            return a;
        }
        if let Opnd::Imm(v) = a {
            // constant fold
            let r = match k {
                CastKind::Zx | CastKind::Trunc => v & from.mask() & to.mask(),
                CastKind::Sx => {
                    let sh = 64 - from.bits();
                    ((((v << sh) as i64) >> sh) as u64) & to.mask()
                }
            };
            return Opnd::Imm(r);
        }
        let d = self.tmp(to);
        self.emit(Op::Cast { k, from, to, d, a });
        Opnd::P(d)
    }
    fn cmp(&mut self, pred: Pred, ty: Ty, a: Opnd, b: Opnd) -> Opnd {
        let d = self.tmp(Ty::I1);
        self.emit(Op::Cmp { pred, ty, d, a, b });
        Opnd::P(d)
    }
    fn sel(&mut self, ty: Ty, c: Opnd, a: Opnd, b: Opnd) -> Opnd {
        let d = self.tmp(ty);
        self.emit(Op::Sel { ty, d, c, a, b });
        Opnd::P(d)
    }
    fn mov_to(&mut self, ty: Ty, d: Place, a: Opnd) {
        self.emit(Op::Mov { ty, d, a });
    }

    pub fn flagdef(&mut self, kind: FlagKind, ty: Ty, mask: u8, a: Opnd, b: Opnd, r: Opnd) {
        self.emit(Op::FlagDef(FlagDef { kind, ty, mask, a, b, r, cin: Opnd::Imm(0), cnt: Opnd::Imm(0), cond_cnt: false }));
    }
    fn flagdef_raw(&mut self, mask: u8, r: Opnd) {
        self.flagdef(FlagKind::Raw, Ty::I32, mask, Opnd::Imm(0), Opnd::Imm(0), r);
    }

    // ------------------------------------------------------------------------------------------ operands
    pub fn reg_ty(&self, r: Register) -> Ty {
        if let Some((_, t, _)) = gpr_part(r) {
            return t;
        }
        Ty::int_of_bytes(r.size() as u32)
    }

    pub fn read_reg(&mut self, r: Register) -> Opnd {
        match gpr_part(r) {
            Some((g, Ty::I32, _)) => self.gpr(g),
            Some((g, ty, sh)) => {
                let w = if sh != 0 { self.bin(BinOp::Shr, Ty::I32, self.gpr(g), Opnd::Imm(sh as u64)) } else { self.gpr(g) };
                self.cast(CastKind::Trunc, Ty::I32, ty, w)
            }
            None => {
                self.unsupported(&format!("read of register {r:?}"));
                Opnd::Imm(0)
            }
        }
    }

    pub fn write_reg(&mut self, r: Register, v: Opnd) {
        match gpr_part(r) {
            Some((g, Ty::I32, _)) => self.mov_to(Ty::I32, Place::Gpr(g), v),
            Some((g, ty, sh)) => self.emit(Op::SetPart { r: g, ty, shift: sh, v }),
            None => self.unsupported(&format!("write of register {r:?}")),
        }
    }

    pub fn mem(&mut self, i: &Instruction) -> Mem {
        let b = i.memory_base();
        let x = i.memory_index();
        let mut m = Mem { base: None, index: None, scale: 1, disp: i.memory_displacement32() as i32, fs: false, cls: MemCls::Unknown, aligned: false };
        if b.is_gpr16() || x.is_gpr16() {
            self.unsupported("16-bit addressing");
        }
        if b != Register::None {
            match gpr_part(b) {
                Some((g, Ty::I32, _)) => m.base = Some(Place::Gpr(g)),
                _ => self.unsupported("base register"),
            }
        }
        if x != Register::None {
            match gpr_part(x) {
                Some((g, Ty::I32, _)) => {
                    m.index = Some(Place::Gpr(g));
                    m.scale = i.memory_index_scale() as u8;
                }
                _ => self.unsupported("index register"),
            }
        }
        match i.segment_prefix() {
            Register::None | Register::DS | Register::ES | Register::SS | Register::CS => {}
            Register::FS => m.fs = true,
            _ => self.unsupported("segment override"),
        }
        if m.base.is_none() && m.index.is_none() {
            m.scale = 1;
        }
        m
    }

    pub fn op_ty(&self, i: &Instruction, n: u32) -> Ty {
        match i.op_kind(n) {
            OpKind::Register => self.reg_ty(i.op_register(n)),
            OpKind::Memory => {
                let s = i.memory_size().size();
                if matches!(s, 1 | 2 | 4 | 8 | 16) {
                    Ty::int_of_bytes(s as u32)
                } else {
                    Ty::I32
                }
            }
            _ => Ty::I32,
        }
    }

    /// read operand n as an integer of `ty`
    pub fn read_op(&mut self, i: &Instruction, n: u32, ty: Ty) -> Opnd {
        match i.op_kind(n) {
            OpKind::Register => self.read_reg(i.op_register(n)),
            OpKind::Memory => {
                let m = self.mem(i);
                let d = self.tmp(ty);
                self.emit(Op::Ld { ty, d, m });
                Opnd::P(d)
            }
            OpKind::Immediate8 | OpKind::Immediate8to16 | OpKind::Immediate8to32 | OpKind::Immediate16 | OpKind::Immediate32 | OpKind::Immediate8_2nd => {
                Opnd::Imm(i.immediate(n) & ty.mask())
            }
            k => {
                self.unsupported(&format!("operand kind {k:?}"));
                Opnd::Imm(0)
            }
        }
    }

    pub fn write_op(&mut self, i: &Instruction, n: u32, ty: Ty, v: Opnd) {
        match i.op_kind(n) {
            OpKind::Register => self.write_reg(i.op_register(n), v),
            OpKind::Memory => {
                let m = self.mem(i);
                self.emit(Op::St { ty, m, v });
            }
            k => self.unsupported(&format!("write operand kind {k:?}")),
        }
    }

    fn push32(&mut self, v: Opnd) {
        let esp = Place::Gpr(Gpr::Esp);
        self.emit(Op::Bin { op: BinOp::Sub, ty: Ty::I32, d: esp, a: Opnd::P(esp), b: Opnd::Imm(4) });
        self.emit(Op::St { ty: Ty::I32, m: Mem::base_disp(esp, 0), v });
    }
    fn pop32(&mut self) -> Opnd {
        let esp = Place::Gpr(Gpr::Esp);
        let t = self.tmp(Ty::I32);
        self.emit(Op::Ld { ty: Ty::I32, d: t, m: Mem::base_disp(esp, 0) });
        self.emit(Op::Bin { op: BinOp::Add, ty: Ty::I32, d: esp, a: Opnd::P(esp), b: Opnd::Imm(4) });
        Opnd::P(t)
    }

    // ------------------------------------------------------------------------------------------ main entry
    pub fn lower(&mut self, i: &Instruction) -> Ctl {
        self.addr = i.ip() as u32;
        self.next = i.next_ip() as u32;
        let lock = i.has_lock_prefix();
        use Mnemonic as M;
        let m = i.mnemonic();
        match m {
            M::Nop | M::Fnop | M::Pause | M::Wait | M::Emms | M::Femms => {
                if m == M::Emms || m == M::Femms {
                    self.emit(Op::Misc(MiscOp::Emms));
                }
                Ctl::Next
            }
            M::Mov => {
                if i.op0_kind() == OpKind::Register && !self.is_gpr_or_known(i.op0_register()) || i.op1_kind() == OpKind::Register && !self.is_gpr_or_known(i.op1_register()) {
                    self.unsupported("mov with non-GPR register");
                    return Ctl::Trap(TRAP_UNSUPPORTED);
                }
                let ty = self.op_ty(i, 0);
                let v = self.read_op(i, 1, ty);
                self.write_op(i, 0, ty, v);
                self.finish()
            }
            M::Movzx | M::Movsx => {
                let dt = self.op_ty(i, 0);
                let st = self.op_ty(i, 1);
                let v = self.read_op(i, 1, st);
                let k = if m == M::Movzx { CastKind::Zx } else { CastKind::Sx };
                let r = self.cast(k, st, dt, v);
                self.write_op(i, 0, dt, r);
                self.finish()
            }
            M::Lea => {
                let mm = self.mem(i);
                let t = self.tmp(Ty::I32);
                self.emit(Op::Lea { d: t, m: mm });
                let dt = self.op_ty(i, 0);
                let r = self.cast(CastKind::Trunc, Ty::I32, dt, Opnd::P(t));
                self.write_op(i, 0, dt, r);
                self.finish()
            }
            M::Xchg => self.l_xchg(i),
            M::Push => {
                if i.op0_kind() == OpKind::Register && i.op0_register().is_segment_register() {
                    self.unsupported("push seg");
                }
                let ty = if i.op0_kind() == OpKind::Memory { self.op_ty(i, 0) } else { self.op_ty(i, 0) };
                if ty != Ty::I32 {
                    self.unsupported("16-bit push");
                    return Ctl::Trap(TRAP_UNSUPPORTED);
                }
                let v = self.read_op(i, 0, Ty::I32);
                // materialise before esp changes when it depends on esp
                let v = if let Opnd::P(Place::Gpr(Gpr::Esp)) = v {
                    let t = self.tmp(Ty::I32);
                    self.mov_to(Ty::I32, t, v);
                    Opnd::P(t)
                } else {
                    v
                };
                self.push32(v);
                self.finish()
            }
            M::Pop => {
                if self.op_ty(i, 0) != Ty::I32 {
                    self.unsupported("16-bit pop");
                    return Ctl::Trap(TRAP_UNSUPPORTED);
                }
                let v = self.pop32();
                self.write_op(i, 0, Ty::I32, v);
                self.finish()
            }
            M::Pushfd => {
                let t = self.tmp(Ty::I32);
                self.emit(Op::FlagsPack { d: t });
                self.push32(Opnd::P(t));
                Ctl::Next
            }
            M::Popfd => {
                let v = self.pop32();
                self.flagdef_raw(FL_ALL, v);
                let b = self.bin(BinOp::Shr, Ty::I32, v, Opnd::Imm(10));
                let b = self.bin(BinOp::And, Ty::I32, b, Opnd::Imm(1));
                self.emit(Op::SetDf(b));
                Ctl::Next
            }
            M::Leave => {
                let esp = Place::Gpr(Gpr::Esp);
                self.mov_to(Ty::I32, esp, self.gpr(Gpr::Ebp));
                let v = self.pop32();
                self.mov_to(Ty::I32, Place::Gpr(Gpr::Ebp), v);
                Ctl::Next
            }
            M::Call => self.l_call(i),
            M::Ret => {
                let pop = if i.op_count() > 0 { i.immediate(0) as u16 } else { 0 };
                Ctl::Ret(pop)
            }
            M::Jmp => self.l_jmp(i),
            M::Jo | M::Jno | M::Jb | M::Jae | M::Je | M::Jne | M::Jbe | M::Ja | M::Js | M::Jns | M::Jp | M::Jnp | M::Jl | M::Jge | M::Jle | M::Jg => {
                let c = cond_of(i.condition_code()).unwrap();
                let t = self.tmp(Ty::I1);
                self.emit(Op::FlagGet { d: t, cond: c });
                Ctl::Jcc(Opnd::P(t), i.near_branch_target() as u32)
            }
            M::Jecxz | M::Jcxz => {
                let (ty, a) = if m == M::Jecxz { (Ty::I32, self.gpr(Gpr::Ecx)) } else { (Ty::I16, self.read_reg(Register::CX)) };
                let c = self.cmp(Pred::Eq, ty, a, Opnd::Imm(0));
                Ctl::Jcc(c, i.near_branch_target() as u32)
            }
            M::Loop | M::Loope | M::Loopne => {
                let ecx = Place::Gpr(Gpr::Ecx);
                self.emit(Op::Bin { op: BinOp::Sub, ty: Ty::I32, d: ecx, a: Opnd::P(ecx), b: Opnd::Imm(1) });
                let mut c = self.cmp(Pred::Ne, Ty::I32, Opnd::P(ecx), Opnd::Imm(0));
                if m != M::Loop {
                    let z = self.tmp(Ty::I1);
                    self.emit(Op::FlagGet { d: z, cond: if m == M::Loope { Cond::E } else { Cond::Ne } });
                    c = self.bin(BinOp::And, Ty::I1, c, Opnd::P(z));
                }
                Ctl::Jcc(c, i.near_branch_target() as u32)
            }
            M::Seto | M::Setno | M::Setb | M::Setae | M::Sete | M::Setne | M::Setbe | M::Seta | M::Sets | M::Setns | M::Setp | M::Setnp | M::Setl | M::Setge | M::Setle | M::Setg => {
                let c = cond_of(i.condition_code()).unwrap();
                let t = self.tmp(Ty::I1);
                self.emit(Op::FlagGet { d: t, cond: c });
                let v = self.cast(CastKind::Zx, Ty::I1, Ty::I8, Opnd::P(t));
                self.write_op(i, 0, Ty::I8, v);
                self.finish()
            }
            M::Cmovo | M::Cmovno | M::Cmovb | M::Cmovae | M::Cmove | M::Cmovne | M::Cmovbe | M::Cmova | M::Cmovs | M::Cmovns | M::Cmovp | M::Cmovnp | M::Cmovl | M::Cmovge | M::Cmovle | M::Cmovg => {
                let c = cond_of(i.condition_code()).unwrap();
                let ty = self.op_ty(i, 0);
                let t = self.tmp(Ty::I1);
                self.emit(Op::FlagGet { d: t, cond: c });
                let src = self.read_op(i, 1, ty);
                let old = self.read_reg(i.op0_register());
                let r = self.sel(ty, Opnd::P(t), src, old);
                self.write_reg(i.op0_register(), r);
                self.finish()
            }
            M::Add | M::Adc | M::Sub | M::Sbb | M::And | M::Or | M::Xor | M::Cmp | M::Test => self.l_alu(i, lock),
            M::Inc | M::Dec => self.l_incdec(i, lock),
            M::Neg => self.l_neg(i, lock),
            M::Not => {
                let ty = self.op_ty(i, 0);
                if lock && i.op0_kind() == OpKind::Memory {
                    let mm = self.mem(i);
                    let old = self.tmp(ty);
                    self.emit(Op::AtomicRmw { op: AtomicOp::Xor, ty, old, m: mm, v: Opnd::Imm(ty.mask()) });
                } else {
                    let a = self.read_op(i, 0, ty);
                    let r = self.un(UnOp::Not, ty, a);
                    self.write_op(i, 0, ty, r);
                }
                self.finish()
            }
            M::Mul | M::Imul => self.l_mul(i),
            M::Div | M::Idiv => self.l_div(i),
            M::Cdq => {
                let r = self.bin(BinOp::Sar, Ty::I32, self.gpr(Gpr::Eax), Opnd::Imm(31));
                self.mov_to(Ty::I32, Place::Gpr(Gpr::Edx), r);
                Ctl::Next
            }
            M::Cwde => {
                let a = self.read_reg(Register::AX);
                let r = self.cast(CastKind::Sx, Ty::I16, Ty::I32, a);
                self.mov_to(Ty::I32, Place::Gpr(Gpr::Eax), r);
                Ctl::Next
            }
            M::Cbw => {
                let a = self.read_reg(Register::AL);
                let r = self.cast(CastKind::Sx, Ty::I8, Ty::I16, a);
                self.write_reg(Register::AX, r);
                Ctl::Next
            }
            M::Cwd => {
                let a = self.read_reg(Register::AX);
                let w = self.cast(CastKind::Sx, Ty::I16, Ty::I32, a);
                let r = self.bin(BinOp::Sar, Ty::I32, w, Opnd::Imm(16));
                let r = self.cast(CastKind::Trunc, Ty::I32, Ty::I16, r);
                self.write_reg(Register::DX, r);
                Ctl::Next
            }
            M::Shl | M::Shr | M::Sar | M::Sal | M::Rol | M::Ror => self.l_shift(i),
            M::Rcl | M::Rcr => self.l_rc(i),
            M::Shld | M::Shrd => self.l_shld(i),
            M::Bt | M::Bts | M::Btr | M::Btc => self.l_bt(i, lock),
            M::Bsf | M::Bsr => self.l_bs(i),
            M::Bswap => {
                let r = i.op0_register();
                let a = self.read_reg(r);
                let v = self.un(UnOp::Bswap, Ty::I32, a);
                self.write_reg(r, v);
                Ctl::Next
            }
            M::Xadd => self.l_xadd(i, lock),
            M::Cmpxchg => self.l_cmpxchg(i),
            M::Lahf => {
                let t = self.tmp(Ty::I32);
                self.emit(Op::FlagsPack { d: t });
                let b = self.cast(CastKind::Trunc, Ty::I32, Ty::I8, Opnd::P(t));
                self.write_reg(Register::AH, b);
                Ctl::Next
            }
            M::Sahf => {
                let a = self.read_reg(Register::AH);
                let w = self.cast(CastKind::Zx, Ty::I8, Ty::I32, a);
                self.flagdef_raw(FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF, w);
                Ctl::Next
            }
            M::Cmc => {
                let t = self.tmp(Ty::I32);
                self.emit(Op::FlagGetBit { d: t, flag: FL_CF });
                let r = self.bin(BinOp::Xor, Ty::I32, Opnd::P(t), Opnd::Imm(1));
                self.flagdef_raw(FL_CF, r);
                Ctl::Next
            }
            M::Clc => {
                self.flagdef_raw(FL_CF, Opnd::Imm(0));
                Ctl::Next
            }
            M::Stc => {
                self.flagdef_raw(FL_CF, Opnd::Imm(1));
                Ctl::Next
            }
            M::Cld => {
                self.emit(Op::SetDf(Opnd::Imm(0)));
                Ctl::Next
            }
            M::Std => {
                self.emit(Op::SetDf(Opnd::Imm(1)));
                Ctl::Next
            }
            M::Movsb | M::Movsw | M::Movsd | M::Stosb | M::Stosw | M::Stosd | M::Lodsb | M::Lodsw | M::Lodsd | M::Scasb | M::Scasw | M::Scasd | M::Cmpsb | M::Cmpsw | M::Cmpsd
                if is_string(i) =>
            {
                self.l_string(i)
            }
            M::Int3 => Ctl::Trap(TRAP_INT3),
            M::Ud2 | M::Ud0 | M::Ud1 => Ctl::Trap(TRAP_UD2),
            M::Cpuid => {
                self.emit(Op::Misc(MiscOp::Cpuid));
                Ctl::Next
            }
            M::Rdtsc => {
                self.emit(Op::Misc(MiscOp::Rdtsc));
                Ctl::Next
            }
            M::Sfence => {
                self.emit(Op::Misc(MiscOp::Sfence));
                Ctl::Next
            }
            M::Lfence => {
                self.emit(Op::Misc(MiscOp::Lfence));
                Ctl::Next
            }
            M::Mfence => {
                self.emit(Op::Misc(MiscOp::Mfence));
                Ctl::Next
            }
            M::Prefetcht0 | M::Prefetcht1 | M::Prefetcht2 | M::Prefetchnta | M::Prefetchw | M::Prefetch => {
                let mm = self.mem(i);
                self.emit(Op::Misc(MiscOp::Prefetch(mm)));
                self.finish()
            }
            _ => {
                if self.lower_x87(i) {
                    return self.finish();
                }
                if self.lower_sse(i) {
                    return self.finish();
                }
                self.unsupported(&format!("{:?}", m));
                Ctl::Trap(TRAP_UNSUPPORTED)
            }
        }
    }

    fn is_gpr_or_known(&self, r: Register) -> bool {
        gpr_part(r).is_some()
    }

    fn finish(&mut self) -> Ctl {
        if self.unsupported.is_some() {
            Ctl::Trap(TRAP_UNSUPPORTED)
        } else {
            Ctl::Next
        }
    }

    // ------------------------------------------------------------------------------------------ pieces
    fn l_xchg(&mut self, i: &Instruction) -> Ctl {
        let ty = self.op_ty(i, 0);
        if i.op0_kind() == OpKind::Memory || i.op1_kind() == OpKind::Memory {
            let (mn, rn) = if i.op0_kind() == OpKind::Memory { (0, 1) } else { (1, 0) };
            let v = self.read_op(i, rn, ty);
            let mm = self.mem(i);
            let old = self.tmp(ty);
            self.emit(Op::AtomicRmw { op: AtomicOp::Xchg, ty, old, m: mm, v });
            let _ = mn;
            self.write_op(i, rn, ty, Opnd::P(old));
        } else {
            let a = self.read_op(i, 0, ty);
            let b = self.read_op(i, 1, ty);
            // snapshot both before writing
            let ta = self.tmp(ty);
            let tb = self.tmp(ty);
            self.mov_to(ty, ta, a);
            self.mov_to(ty, tb, b);
            self.write_op(i, 0, ty, Opnd::P(tb));
            self.write_op(i, 1, ty, Opnd::P(ta));
        }
        self.finish()
    }

    fn l_call(&mut self, i: &Instruction) -> Ctl {
        let ret_addr = self.next;
        let target = match i.op0_kind() {
            OpKind::NearBranch32 => {
                let t = i.near_branch_target() as u32;
                if t == ret_addr {
                    // call $+5 ; pop reg idiom
                    self.push32(Opnd::Imm(ret_addr as u64));
                    return Ctl::Next;
                }
                CallTarget::Direct(t)
            }
            OpKind::Register => {
                let v = self.read_reg(i.op0_register());
                CallTarget::Indirect(v)
            }
            OpKind::Memory => {
                let mm = self.mem(i);
                if mm.base.is_none() && mm.index.is_none() && !mm.fs {
                    if let Some(t) = self.env.iat_target(mm.disp as u32) {
                        return self.emit_call(t, ret_addr);
                    }
                }
                let t = self.tmp(Ty::I32);
                self.emit(Op::Ld { ty: Ty::I32, d: t, m: mm });
                CallTarget::Indirect(Opnd::P(t))
            }
            k => {
                self.unsupported(&format!("call operand {k:?}"));
                return Ctl::Trap(TRAP_UNSUPPORTED);
            }
        };
        // an indirect target held in a register that is esp-dependent is not expected
        self.emit_call(target, ret_addr)
    }

    fn emit_call(&mut self, target: CallTarget, ret_addr: u32) -> Ctl {
        let nr = self.env.is_noreturn(&target);
        self.emit(Op::Call { target, ret_addr });
        if nr {
            Ctl::Stop
        } else {
            Ctl::Next
        }
    }

    fn l_jmp(&mut self, i: &Instruction) -> Ctl {
        match i.op0_kind() {
            OpKind::NearBranch32 => Ctl::Jmp(i.near_branch_target() as u32),
            OpKind::Register => {
                let v = self.read_reg(i.op0_register());
                Ctl::TailInd(CallTarget::Indirect(v))
            }
            OpKind::Memory => {
                let mm = self.mem(i);
                if mm.base.is_none() && mm.index.is_none() && !mm.fs {
                    if let Some(t) = self.env.iat_target(mm.disp as u32) {
                        return Ctl::TailInd(t);
                    }
                }
                if mm.base.is_none() && mm.scale == 4 && mm.index.is_some() && !mm.fs {
                    if let Some(tg) = self.env.jump_table(self.addr) {
                        let idx = Opnd::P(mm.index.unwrap());
                        return Ctl::Switch(idx, tg);
                    }
                }
                let t = self.tmp(Ty::I32);
                self.emit(Op::Ld { ty: Ty::I32, d: t, m: mm });
                Ctl::TailInd(CallTarget::Indirect(Opnd::P(t)))
            }
            k => {
                self.unsupported(&format!("jmp operand {k:?}"));
                Ctl::Trap(TRAP_UNSUPPORTED)
            }
        }
    }

    fn l_alu(&mut self, i: &Instruction, lock: bool) -> Ctl {
        let m = i.mnemonic();
        use Mnemonic as M;
        let ty = self.op_ty(i, 0);
        let writes = !matches!(m, M::Cmp | M::Test);
        let atomic = lock && i.op0_kind() == OpKind::Memory && writes;
        let b = self.read_op(i, 1, ty);
        let same_reg = i.op0_kind() == OpKind::Register && i.op1_kind() == OpKind::Register && i.op0_register() == i.op1_register();
        let (a, old_for_atomic) = if atomic {
            (Opnd::Imm(0), true)
        } else if same_reg {
            (b, false)
        } else {
            (self.read_op(i, 0, ty), false)
        };
        let _ = old_for_atomic;
        let cin = if matches!(m, M::Adc | M::Sbb) {
            let c = self.tmp(Ty::I32);
            self.emit(Op::FlagGetBit { d: c, flag: FL_CF });
            Opnd::P(c)
        } else {
            Opnd::Imm(0)
        };
        let (a, r) = if atomic {
            // lock-prefixed RMW: atomic op yields the old value, result recomputed for flags
            let aop = match m {
                M::Add => AtomicOp::Add,
                M::Sub => AtomicOp::Sub,
                M::And => AtomicOp::And,
                M::Or => AtomicOp::Or,
                M::Xor => AtomicOp::Xor,
                _ => {
                    self.unsupported("locked adc/sbb");
                    return Ctl::Trap(TRAP_UNSUPPORTED);
                }
            };
            let mm = self.mem(i);
            let old = self.tmp(ty);
            self.emit(Op::AtomicRmw { op: aop, ty, old, m: mm, v: b });
            let bop = match m {
                M::Add => BinOp::Add,
                M::Sub => BinOp::Sub,
                M::And => BinOp::And,
                M::Or => BinOp::Or,
                _ => BinOp::Xor,
            };
            let r = self.bin(bop, ty, Opnd::P(old), b);
            (Opnd::P(old), r)
        } else {
            let r = match m {
                M::Add => self.bin(BinOp::Add, ty, a, b),
                M::Adc => {
                    let t = self.bin(BinOp::Add, ty, a, b);
                    let c = self.cast(CastKind::Trunc, Ty::I32, ty, cin);
                    self.bin(BinOp::Add, ty, t, c)
                }
                M::Sub | M::Cmp => self.bin(BinOp::Sub, ty, a, b),
                M::Sbb => {
                    let t = self.bin(BinOp::Sub, ty, a, b);
                    let c = self.cast(CastKind::Trunc, Ty::I32, ty, cin);
                    self.bin(BinOp::Sub, ty, t, c)
                }
                M::And | M::Test => self.bin(BinOp::And, ty, a, b),
                M::Or => self.bin(BinOp::Or, ty, a, b),
                M::Xor => self.bin(BinOp::Xor, ty, a, b),
                _ => unreachable!(),
            };
            (a, r)
        };
        let kind = match m {
            M::Add => FlagKind::Add,
            M::Adc => FlagKind::Adc,
            M::Sub | M::Cmp => FlagKind::Sub,
            M::Sbb => FlagKind::Sbb,
            _ => FlagKind::Logic,
        };
        // logic-kind flags depend on the result only
        let (fa, fb) = if kind == FlagKind::Logic { (Opnd::Imm(0), Opnd::Imm(0)) } else { (a, b) };
        self.emit(Op::FlagDef(FlagDef { kind, ty, mask: FL_ALL, a: fa, b: fb, r, cin, cnt: Opnd::Imm(0), cond_cnt: false }));
        if writes && !atomic {
            self.write_op(i, 0, ty, r);
        }
        self.finish()
    }

    fn l_incdec(&mut self, i: &Instruction, lock: bool) -> Ctl {
        let ty = self.op_ty(i, 0);
        let inc = i.mnemonic() == Mnemonic::Inc;
        let (a, r) = if lock && i.op0_kind() == OpKind::Memory {
            let mm = self.mem(i);
            let old = self.tmp(ty);
            self.emit(Op::AtomicRmw { op: if inc { AtomicOp::Add } else { AtomicOp::Sub }, ty, old, m: mm, v: Opnd::Imm(1) });
            let r = self.bin(if inc { BinOp::Add } else { BinOp::Sub }, ty, Opnd::P(old), Opnd::Imm(1));
            (Opnd::P(old), r)
        } else {
            let a = self.read_op(i, 0, ty);
            let r = self.bin(if inc { BinOp::Add } else { BinOp::Sub }, ty, a, Opnd::Imm(1));
            self.flagdef(if inc { FlagKind::Inc } else { FlagKind::Dec }, ty, FL_ALL & !FL_CF, a, Opnd::Imm(1), r);
            self.write_op(i, 0, ty, r);
            return self.finish();
        };
        self.flagdef(if inc { FlagKind::Inc } else { FlagKind::Dec }, ty, FL_ALL & !FL_CF, a, Opnd::Imm(1), r);
        self.finish()
    }

    fn l_neg(&mut self, i: &Instruction, lock: bool) -> Ctl {
        let ty = self.op_ty(i, 0);
        let (a, r) = if lock && i.op0_kind() == OpKind::Memory {
            self.unsupported("locked neg");
            return Ctl::Trap(TRAP_UNSUPPORTED);
        } else {
            let a = self.read_op(i, 0, ty);
            let r = self.un(UnOp::Neg, ty, a);
            // neg: flags of (0 - a); recorded before the destination is overwritten
            self.flagdef(FlagKind::Sub, ty, FL_ALL, Opnd::Imm(0), a, r);
            self.write_op(i, 0, ty, r);
            return self.finish();
        };
        self.flagdef(FlagKind::Sub, ty, FL_ALL, Opnd::Imm(0), a, r);
        self.finish()
    }

    fn l_mul(&mut self, i: &Instruction) -> Ctl {
        let signed = i.mnemonic() == Mnemonic::Imul;
        match i.op_count() {
            1 => {
                let ty = self.op_ty(i, 0);
                let b = self.read_op(i, 0, ty);
                let (acc, hi_p, lo_p): (Opnd, Place, Place);
                let (hi, lo) = (self.tmp(ty), self.tmp(ty));
                match ty {
                    Ty::I8 => {
                        acc = self.read_reg(Register::AL);
                        hi_p = hi;
                        lo_p = lo;
                    }
                    Ty::I16 => {
                        acc = self.read_reg(Register::AX);
                        hi_p = hi;
                        lo_p = lo;
                    }
                    _ => {
                        acc = self.gpr(Gpr::Eax);
                        hi_p = hi;
                        lo_p = lo;
                    }
                }
                self.emit(Op::MulWide { signed, ty, hi: hi_p, lo: lo_p, a: acc, b });
                // CF=OF = hi != (signed ? sext(lo) : 0)
                let ovf = if signed {
                    let s = self.bin(BinOp::Sar, ty, Opnd::P(lo), Opnd::Imm(ty.bits() as u64 - 1));
                    self.cmp(Pred::Ne, ty, Opnd::P(hi), s)
                } else {
                    self.cmp(Pred::Ne, ty, Opnd::P(hi), Opnd::Imm(0))
                };
                let o32 = self.cast(CastKind::Zx, Ty::I1, Ty::I32, ovf);
                let fl = self.bin(BinOp::Mul, Ty::I32, o32, Opnd::Imm(0x801));
                self.flagdef_raw(FL_CF | FL_OF, fl);
                match ty {
                    Ty::I8 => {
                        // ax = hi:lo
                        let h = self.cast(CastKind::Zx, Ty::I8, Ty::I16, Opnd::P(hi));
                        let h = self.bin(BinOp::Shl, Ty::I16, h, Opnd::Imm(8));
                        let l = self.cast(CastKind::Zx, Ty::I8, Ty::I16, Opnd::P(lo));
                        let v = self.bin(BinOp::Or, Ty::I16, h, l);
                        self.write_reg(Register::AX, v);
                    }
                    Ty::I16 => {
                        self.write_reg(Register::AX, Opnd::P(lo));
                        self.write_reg(Register::DX, Opnd::P(hi));
                    }
                    _ => {
                        self.mov_to(Ty::I32, Place::Gpr(Gpr::Eax), Opnd::P(lo));
                        self.mov_to(Ty::I32, Place::Gpr(Gpr::Edx), Opnd::P(hi));
                    }
                }
                self.finish()
            }
            n => {
                // imul r, r/m [, imm]
                let ty = self.op_ty(i, 0);
                let (a, b) = if n == 2 { (self.read_op(i, 0, ty), self.read_op(i, 1, ty)) } else { (self.read_op(i, 1, ty), self.read_op(i, 2, ty)) };
                let wide = if ty == Ty::I16 { Ty::I32 } else { Ty::I64 };
                let wa = self.cast(CastKind::Sx, ty, wide, a);
                let wb = self.cast(CastKind::Sx, ty, wide, b);
                let p = self.bin(BinOp::Mul, wide, wa, wb);
                let lo = self.cast(CastKind::Trunc, wide, ty, p);
                let back = self.cast(CastKind::Sx, ty, wide, lo);
                let ovf = self.cmp(Pred::Ne, wide, p, back);
                let o32 = self.cast(CastKind::Zx, Ty::I1, Ty::I32, ovf);
                let fl = self.bin(BinOp::Mul, Ty::I32, o32, Opnd::Imm(0x801));
                self.flagdef_raw(FL_CF | FL_OF, fl);
                self.write_op(i, 0, ty, lo);
                self.finish()
            }
        }
    }

    fn l_div(&mut self, i: &Instruction) -> Ctl {
        let signed = i.mnemonic() == Mnemonic::Idiv;
        let ty = self.op_ty(i, 0);
        let d = self.read_op(i, 0, ty);
        let (q, r) = (self.tmp(ty), self.tmp(ty));
        match ty {
            Ty::I8 => {
                let ax = self.read_reg(Register::AX);
                let lo = self.cast(CastKind::Trunc, Ty::I16, Ty::I8, ax);
                let hi0 = self.bin(BinOp::Shr, Ty::I16, ax, Opnd::Imm(8));
                let hi = self.cast(CastKind::Trunc, Ty::I16, Ty::I8, hi0);
                self.emit(Op::DivWide { signed, ty, q, r, hi, lo, div: d });
                self.write_reg(Register::AL, Opnd::P(q));
                self.write_reg(Register::AH, Opnd::P(r));
            }
            Ty::I16 => {
                let lo = self.read_reg(Register::AX);
                let hi = self.read_reg(Register::DX);
                self.emit(Op::DivWide { signed, ty, q, r, hi, lo, div: d });
                self.write_reg(Register::AX, Opnd::P(q));
                self.write_reg(Register::DX, Opnd::P(r));
            }
            _ => {
                let lo = self.gpr(Gpr::Eax);
                let hi = self.gpr(Gpr::Edx);
                self.emit(Op::DivWide { signed, ty, q, r, hi, lo, div: d });
                self.mov_to(Ty::I32, Place::Gpr(Gpr::Eax), Opnd::P(q));
                self.mov_to(Ty::I32, Place::Gpr(Gpr::Edx), Opnd::P(r));
            }
        }
        self.finish()
    }

    fn shift_count(&mut self, i: &Instruction, n: u32) -> (Opnd, bool) {
        // returns (count masked to 5 bits, count may be zero at run time)
        match i.op_kind(n) {
            OpKind::Register => {
                let c = self.read_reg(i.op_register(n));
                let c = self.cast(CastKind::Zx, Ty::I8, Ty::I32, c);
                let c = self.bin(BinOp::And, Ty::I32, c, Opnd::Imm(31));
                (c, true)
            }
            _ => (Opnd::Imm(i.immediate(n) & 31), false),
        }
    }

    fn l_shift(&mut self, i: &Instruction) -> Ctl {
        use Mnemonic as M;
        let ty = self.op_ty(i, 0);
        let (cnt, var) = if i.op_count() >= 2 { self.shift_count(i, 1) } else { (Opnd::Imm(1), false) };
        if let Opnd::Imm(0) = cnt {
            return Ctl::Next; // no effect at all
        }
        let a = self.read_op(i, 0, ty);
        let m = i.mnemonic();
        let (bop, fk) = match m {
            M::Shl | M::Sal => (BinOp::Shl, FlagKind::Shl),
            M::Shr => (BinOp::Shr, FlagKind::Shr),
            M::Sar => (BinOp::Sar, FlagKind::Sar),
            M::Rol => (BinOp::Rol, FlagKind::Rol),
            _ => (BinOp::Ror, FlagKind::Ror),
        };
        let r = self.bin(bop, ty, a, cnt);
        let mask = match fk {
            FlagKind::Rol | FlagKind::Ror => FL_CF | FL_OF,
            _ => FL_CF | FL_PF | FL_ZF | FL_SF | FL_OF,
        };
        self.emit(Op::FlagDef(FlagDef { kind: fk, ty, mask, a, b: Opnd::Imm(0), r, cin: Opnd::Imm(0), cnt, cond_cnt: var }));
        self.write_op(i, 0, ty, r);
        self.finish()
    }

    fn l_rc(&mut self, i: &Instruction) -> Ctl {
        // rcl/rcr via 64-bit arithmetic on (CF:value)
        let left = i.mnemonic() == Mnemonic::Rcl;
        let ty = self.op_ty(i, 0);
        let bits = ty.bits() as u64;
        let (cnt0, var) = if i.op_count() >= 2 { self.shift_count(i, 1) } else { (Opnd::Imm(1), false) };
        if var {
            self.unsupported("rcl/rcr with variable count");
            return Ctl::Trap(TRAP_UNSUPPORTED);
        }
        if let Opnd::Imm(0) = cnt0 {
            return Ctl::Next;
        }
        let a = self.read_op(i, 0, ty);
        let cf = self.tmp(Ty::I32);
        self.emit(Op::FlagGetBit { d: cf, flag: FL_CF });
        // count modulo (bits+1)
        let cnt = self.bin(BinOp::URem, Ty::I32, cnt0, Opnd::Imm(bits + 1));
        // wide = (cf << bits) | a   as I64 ; rotate within (bits+1) bits
        let a64 = self.cast(CastKind::Zx, ty, Ty::I64, a);
        let cf64 = self.cast(CastKind::Zx, Ty::I32, Ty::I64, Opnd::P(cf));
        let cfs = self.bin(BinOp::Shl, Ty::I64, cf64, Opnd::Imm(bits));
        let w = self.bin(BinOp::Or, Ty::I64, cfs, a64);
        let width = bits + 1;
        let cnt64 = self.cast(CastKind::Zx, Ty::I32, Ty::I64, cnt);
        let (res_w, ) = {
            let (l, r_) = if left {
                let l = self.bin(BinOp::Shl, Ty::I64, w, cnt64);
                let inv = self.bin(BinOp::Sub, Ty::I64, Opnd::Imm(width), cnt64);
                let r_ = self.bin(BinOp::Shr, Ty::I64, w, inv);
                (l, r_)
            } else {
                let l = self.bin(BinOp::Shr, Ty::I64, w, cnt64);
                let inv = self.bin(BinOp::Sub, Ty::I64, Opnd::Imm(width), cnt64);
                let r_ = self.bin(BinOp::Shl, Ty::I64, w, inv);
                (l, r_)
            };
            let o = self.bin(BinOp::Or, Ty::I64, l, r_);
            // with cnt == 0 (after modulo) r_ shifts by width: result must equal w
            let z = self.cmp(Pred::Eq, Ty::I32, cnt, Opnd::Imm(0));
            let o = self.sel(Ty::I64, z, w, o);
            let wmask = (1u64 << width) - 1;
            (self.bin(BinOp::And, Ty::I64, o, Opnd::Imm(wmask)),)
        };
        let r = self.cast(CastKind::Trunc, Ty::I64, ty, res_w);
        let newcf = self.bin(BinOp::Shr, Ty::I64, res_w, Opnd::Imm(bits));
        let newcf32 = self.cast(CastKind::Trunc, Ty::I64, Ty::I32, newcf);
        // OF: rcl: msb(r) ^ CF_new ; rcr: msb(r) ^ msb(r<<1) i.e. top two bits of r
        let msb = self.bin(BinOp::Shr, ty, r, Opnd::Imm(bits - 1));
        let msb32 = self.cast(CastKind::Zx, ty, Ty::I32, msb);
        let of = if left {
            self.bin(BinOp::Xor, Ty::I32, msb32, newcf32)
        } else {
            let nx = self.bin(BinOp::Shr, ty, r, Opnd::Imm(bits - 2));
            let nx32 = self.cast(CastKind::Zx, ty, Ty::I32, nx);
            let nx1 = self.bin(BinOp::And, Ty::I32, nx32, Opnd::Imm(1));
            self.bin(BinOp::Xor, Ty::I32, msb32, nx1)
        };
        let ofs = self.bin(BinOp::Shl, Ty::I32, of, Opnd::Imm(11));
        let raw = self.bin(BinOp::Or, Ty::I32, newcf32, ofs);
        self.flagdef_raw(FL_CF | FL_OF, raw);
        self.write_op(i, 0, ty, r);
        self.finish()
    }

    fn l_shld(&mut self, i: &Instruction) -> Ctl {
        let left = i.mnemonic() == Mnemonic::Shld;
        let ty = self.op_ty(i, 0);
        if ty != Ty::I32 {
            self.unsupported("16-bit shld/shrd");
            return Ctl::Trap(TRAP_UNSUPPORTED);
        }
        let (cnt, var) = self.shift_count(i, 2);
        if let Opnd::Imm(0) = cnt {
            return Ctl::Next;
        }
        let a = self.read_op(i, 0, ty);
        let b = self.read_op(i, 1, ty);
        // compute in 64 bits: shld: ((a:b) << cnt) >> 32 ; shrd: ((b:a) >> cnt) & 0xffffffff
        let a64 = self.cast(CastKind::Zx, Ty::I32, Ty::I64, a);
        let b64 = self.cast(CastKind::Zx, Ty::I32, Ty::I64, b);
        let cnt64 = self.cast(CastKind::Zx, Ty::I32, Ty::I64, cnt);
        let r = if left {
            let hi = self.bin(BinOp::Shl, Ty::I64, a64, Opnd::Imm(32));
            let w = self.bin(BinOp::Or, Ty::I64, hi, b64);
            let s = self.bin(BinOp::Shl, Ty::I64, w, cnt64);
            let s = self.bin(BinOp::Shr, Ty::I64, s, Opnd::Imm(32));
            self.cast(CastKind::Trunc, Ty::I64, Ty::I32, s)
        } else {
            let hi = self.bin(BinOp::Shl, Ty::I64, b64, Opnd::Imm(32));
            let w = self.bin(BinOp::Or, Ty::I64, hi, a64);
            let s = self.bin(BinOp::Shr, Ty::I64, w, cnt64);
            self.cast(CastKind::Trunc, Ty::I64, Ty::I32, s)
        };
        let r = if var {
            // count 0 leaves the destination unchanged
            let nz = nz_place(self, cnt);
            self.sel(Ty::I32, Opnd::P(nz), r, a)
        } else {
            r
        };
        self.emit(Op::FlagDef(FlagDef {
            kind: if left { FlagKind::Shl } else { FlagKind::Shr },
            ty,
            mask: FL_CF | FL_PF | FL_ZF | FL_SF | FL_OF,
            a,
            b: Opnd::Imm(0),
            r,
            cin: Opnd::Imm(0),
            cnt,
            cond_cnt: var,
        }));
        self.write_op(i, 0, ty, r);
        self.finish()
    }

    fn l_bt(&mut self, i: &Instruction, lock: bool) -> Ctl {
        use Mnemonic as M;
        let m = i.mnemonic();
        let ty = self.op_ty(i, 0);
        let bits = ty.bits() as u64;
        let _ = lock;
        // bit offset
        let off = self.read_op(i, 1, ty);
        let off32 = self.cast(CastKind::Zx, ty, Ty::I32, off);
        let (val, bit, mem_ref): (Opnd, Opnd, Option<Mem>) = if i.op0_kind() == OpKind::Memory && i.op1_kind() == OpKind::Register {
            // bit string: address advances by (signed offset >> log2(bits)) * (bits/8)
            let mut mm = self.mem(i);
            let soff = self.cast(CastKind::Sx, ty, Ty::I32, off);
            let sh = if bits == 32 { 5 } else { 4 };
            let word = self.bin(BinOp::Sar, Ty::I32, soff, Opnd::Imm(sh));
            let bytes = self.bin(BinOp::Mul, Ty::I32, word, Opnd::Imm(bits / 8));
            let base_t = self.tmp(Ty::I32);
            self.emit(Op::Lea { d: base_t, m: mm });
            let addr = self.bin(BinOp::Add, Ty::I32, Opnd::P(base_t), bytes);
            let at = addr.place().unwrap();
            mm = Mem::base_disp(at, 0);
            let d = self.tmp(ty);
            self.emit(Op::Ld { ty, d, m: mm });
            let bitn = self.bin(BinOp::And, Ty::I32, off32, Opnd::Imm(bits - 1));
            (Opnd::P(d), bitn, Some(mm))
        } else {
            let v = self.read_op(i, 0, ty);
            let bitn = self.bin(BinOp::And, Ty::I32, off32, Opnd::Imm(bits - 1));
            (v, bitn, if i.op0_kind() == OpKind::Memory { Some(self.mem(i)) } else { None })
        };
        let v32 = self.cast(CastKind::Zx, ty, Ty::I32, val);
        let sh = self.bin(BinOp::Shr, Ty::I32, v32, bit);
        let cf = self.bin(BinOp::And, Ty::I32, sh, Opnd::Imm(1));
        self.flagdef_raw(FL_CF, cf);
        if m != M::Bt {
            let one = self.bin(BinOp::Shl, Ty::I32, Opnd::Imm(1), bit);
            let nv = match m {
                M::Bts => self.bin(BinOp::Or, Ty::I32, v32, one),
                M::Btr => {
                    let n = self.un(UnOp::Not, Ty::I32, one);
                    self.bin(BinOp::And, Ty::I32, v32, n)
                }
                _ => self.bin(BinOp::Xor, Ty::I32, v32, one),
            };
            let nv = self.cast(CastKind::Trunc, Ty::I32, ty, nv);
            if let Some(mm) = mem_ref {
                self.emit(Op::St { ty, m: mm, v: nv });
            } else {
                self.write_op(i, 0, ty, nv);
            }
        }
        self.finish()
    }

    fn l_bs(&mut self, i: &Instruction) -> Ctl {
        let ty = self.op_ty(i, 0);
        let src = self.read_op(i, 1, ty);
        let s32 = self.cast(CastKind::Zx, ty, Ty::I32, src);
        let z = self.cmp(Pred::Eq, Ty::I32, s32, Opnd::Imm(0));
        let idx = if i.mnemonic() == Mnemonic::Bsf {
            self.un(UnOp::Ctz, Ty::I32, s32)
        } else {
            let c = self.un(UnOp::Clz, Ty::I32, s32);
            self.bin(BinOp::Sub, Ty::I32, Opnd::Imm(31), c)
        };
        let old = self.read_reg(i.op0_register());
        let old32 = self.cast(CastKind::Zx, ty, Ty::I32, old);
        let r32 = self.sel(Ty::I32, z, old32, idx);
        let r = self.cast(CastKind::Trunc, Ty::I32, ty, r32);
        let zf = self.cast(CastKind::Zx, Ty::I1, Ty::I32, z);
        let zf = self.bin(BinOp::Shl, Ty::I32, zf, Opnd::Imm(6));
        self.flagdef_raw(FL_ZF, zf);
        self.write_reg(i.op0_register(), r);
        self.finish()
    }

    fn l_xadd(&mut self, i: &Instruction, lock: bool) -> Ctl {
        let ty = self.op_ty(i, 0);
        let src = self.read_op(i, 1, ty);
        let (a, r) = if i.op0_kind() == OpKind::Memory {
            let mm = self.mem(i);
            let old = self.tmp(ty);
            self.emit(Op::AtomicRmw { op: AtomicOp::Add, ty, old, m: mm, v: src });
            let r = self.bin(BinOp::Add, ty, Opnd::P(old), src);
            (Opnd::P(old), r)
        } else {
            let a0 = self.read_op(i, 0, ty);
            let a = self.tmp(ty);
            self.mov_to(ty, a, a0);
            let a = Opnd::P(a);
            let r = self.bin(BinOp::Add, ty, a, src);
            self.flagdef(FlagKind::Add, ty, FL_ALL, a, src, r);
            // when dest and src are the same register the result of the add wins
            self.write_op(i, 1, ty, a);
            self.write_op(i, 0, ty, r);
            return self.finish();
        };
        let _ = lock;
        self.flagdef(FlagKind::Add, ty, FL_ALL, a, src, r);
        self.write_op(i, 1, ty, a);
        self.finish()
    }

    fn l_cmpxchg(&mut self, i: &Instruction) -> Ctl {
        let ty = self.op_ty(i, 0);
        let acc_reg = match ty {
            Ty::I8 => Register::AL,
            Ty::I16 => Register::AX,
            _ => Register::EAX,
        };
        let exp = self.read_reg(acc_reg);
        let new = self.read_op(i, 1, ty);
        let old = self.tmp(ty);
        if i.op0_kind() == OpKind::Memory {
            let mm = self.mem(i);
            self.emit(Op::AtomicCas { ty, old, m: mm, expected: exp, new });
        } else {
            let cur = self.read_op(i, 0, ty);
            self.mov_to(ty, old, cur);
            let eq = self.cmp(Pred::Eq, ty, cur, exp);
            let nv = self.sel(ty, eq, new, cur);
            self.write_op(i, 0, ty, nv);
        }
        let r = self.bin(BinOp::Sub, ty, exp, Opnd::P(old));
        self.flagdef(FlagKind::Sub, ty, FL_ALL, exp, Opnd::P(old), r);
        self.write_reg(acc_reg, Opnd::P(old));
        self.finish()
    }

    fn l_string(&mut self, i: &Instruction) -> Ctl {
        use Mnemonic as M;
        let m = i.mnemonic();
        let (kind, size) = match m {
            M::Movsb => (StrKind::Movs, 1),
            M::Movsw => (StrKind::Movs, 2),
            M::Movsd => (StrKind::Movs, 4),
            M::Stosb => (StrKind::Stos, 1),
            M::Stosw => (StrKind::Stos, 2),
            M::Stosd => (StrKind::Stos, 4),
            M::Lodsb => (StrKind::Lods, 1),
            M::Lodsw => (StrKind::Lods, 2),
            M::Lodsd => (StrKind::Lods, 4),
            M::Scasb => (StrKind::Scas, 1),
            M::Scasw => (StrKind::Scas, 2),
            M::Scasd => (StrKind::Scas, 4),
            M::Cmpsb => (StrKind::Cmps, 1),
            M::Cmpsw => (StrKind::Cmps, 2),
            _ => (StrKind::Cmps, 4),
        };
        let rep = if i.has_rep_prefix() || i.has_repe_prefix() {
            if matches!(kind, StrKind::Cmps | StrKind::Scas) {
                Rep::Repe
            } else {
                Rep::Rep
            }
        } else if i.has_repne_prefix() {
            Rep::Repne
        } else {
            Rep::None
        };
        if i.segment_prefix() != Register::None && i.segment_prefix() != Register::DS && i.segment_prefix() != Register::ES {
            self.unsupported("segment override on string op");
            return Ctl::Trap(TRAP_UNSUPPORTED);
        }
        let ty = Ty::int_of_bytes(size as u32);
        let (ta, tb, done) = if matches!(kind, StrKind::Cmps | StrKind::Scas) {
            (self.tmp(ty), self.tmp(ty), self.tmp(Ty::I32))
        } else {
            (Place::Tmp(0), Place::Tmp(0), Place::Tmp(0))
        };
        self.emit(Op::Str { kind, size, rep, src_local: false, dst_local: false, ta, tb, done });
        if matches!(kind, StrKind::Cmps | StrKind::Scas) {
            let r = self.bin(BinOp::Sub, ty, Opnd::P(ta), Opnd::P(tb));
            self.emit(Op::FlagDef(FlagDef { kind: FlagKind::Sub, ty, mask: FL_ALL, a: Opnd::P(ta), b: Opnd::P(tb), r, cin: Opnd::Imm(0), cnt: Opnd::P(done), cond_cnt: rep != Rep::None }));
        }
        Ctl::Next
    }
}

fn nz_place(l: &mut Lower, cnt: Opnd) -> Place {
    let d = l.tmp(Ty::I1);
    l.emit(Op::Cmp { pred: Pred::Ne, ty: Ty::I32, d, a: cnt, b: Opnd::Imm(0) });
    d
}

pub fn is_string(i: &Instruction) -> bool {
    matches!(
        i.code(),
        Code::Movsb_m8_m8 | Code::Movsw_m16_m16 | Code::Movsd_m32_m32 | Code::Stosb_m8_AL | Code::Stosw_m16_AX | Code::Stosd_m32_EAX | Code::Lodsb_AL_m8 | Code::Lodsw_AX_m16 | Code::Lodsd_EAX_m32 | Code::Scasb_AL_m8 | Code::Scasw_AX_m16 | Code::Scasd_EAX_m32 | Code::Cmpsb_m8_m8 | Code::Cmpsw_m16_m16 | Code::Cmpsd_m32_m32
    )
}
