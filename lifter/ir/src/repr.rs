//! The lifter IR. Straight-line three-address operations over typed places, with explicit lazy-flag producers/consumers.
//! Semantics of every x86 instruction are defined once, in `lower.rs`; backends (the C emitter) only map these ops.

use std::fmt;

pub type Tmp = u32;

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Ty {
    I1,
    I8,
    I16,
    I32,
    I64,
    F32,
    F64,
    V128,
}

impl Ty {
    pub fn bits(self) -> u32 {
        match self {
            Ty::I1 => 1,
            Ty::I8 => 8,
            Ty::I16 => 16,
            Ty::I32 | Ty::F32 => 32,
            Ty::I64 | Ty::F64 => 64,
            Ty::V128 => 128,
        }
    }
    pub fn bytes(self) -> u32 {
        (self.bits() + 7) / 8
    }
    pub fn int_of_bytes(n: u32) -> Ty {
        match n {
            1 => Ty::I8,
            2 => Ty::I16,
            4 => Ty::I32,
            8 => Ty::I64,
            16 => Ty::V128,
            _ => panic!("bad size {n}"),
        }
    }
    pub fn mask(self) -> u64 {
        match self {
            Ty::I1 => 1,
            Ty::I8 => 0xff,
            Ty::I16 => 0xffff,
            Ty::I32 | Ty::F32 => 0xffff_ffff,
            _ => u64::MAX,
        }
    }
    pub fn is_float(self) -> bool {
        matches!(self, Ty::F32 | Ty::F64)
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
#[repr(u8)]
pub enum Gpr {
    Eax = 0,
    Ecx = 1,
    Edx = 2,
    Ebx = 3,
    Esp = 4,
    Ebp = 5,
    Esi = 6,
    Edi = 7,
}

pub const GPRS: [Gpr; 8] = [Gpr::Eax, Gpr::Ecx, Gpr::Edx, Gpr::Ebx, Gpr::Esp, Gpr::Ebp, Gpr::Esi, Gpr::Edi];

impl Gpr {
    pub fn name(self) -> &'static str {
        ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"][self as usize]
    }
    pub fn from_idx(i: usize) -> Gpr {
        GPRS[i]
    }
    pub fn is_callee_saved(self) -> bool {
        matches!(self, Gpr::Ebx | Gpr::Esi | Gpr::Edi | Gpr::Ebp)
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Place {
    Gpr(Gpr),
    Tmp(Tmp),
    Xmm(u8),
    Mm(u8),
    /// x87 register relative to the current top-of-stack (before the x87 resolution pass)
    St(u8),
    /// x87 absolute slot (after resolution)
    Fp(u8),
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum Opnd {
    P(Place),
    Imm(u64),
}

impl Opnd {
    pub fn gpr(g: Gpr) -> Opnd {
        Opnd::P(Place::Gpr(g))
    }
    pub fn tmp(t: Tmp) -> Opnd {
        Opnd::P(Place::Tmp(t))
    }
    pub fn place(&self) -> Option<Place> {
        match self {
            Opnd::P(p) => Some(*p),
            _ => None,
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum MemCls {
    /// not yet classified; the emitter treats this as Shared
    Unknown,
    /// thread-private: stack / fs: / proven stack-derived pointer
    Local,
    /// possibly visible to other threads
    Shared,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub struct Mem {
    pub base: Option<Place>,
    pub index: Option<Place>,
    pub scale: u8,
    pub disp: i32,
    /// fs: override (address is relative to the TEB)
    pub fs: bool,
    pub cls: MemCls,
    /// provably naturally aligned for the access size
    pub aligned: bool,
}

impl Mem {
    pub fn abs(addr: u32) -> Mem {
        Mem { base: None, index: None, scale: 1, disp: addr as i32, fs: false, cls: MemCls::Unknown, aligned: false }
    }
    pub fn base_disp(b: Place, disp: i32) -> Mem {
        Mem { base: Some(b), index: None, scale: 1, disp, fs: false, cls: MemCls::Unknown, aligned: false }
    }
    pub fn with_disp(&self, add: i32) -> Mem {
        let mut m = *self;
        m.disp = m.disp.wrapping_add(add);
        m
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum BinOp {
    Add,
    Sub,
    And,
    Or,
    Xor,
    Mul,
    Shl,
    Shr,
    Sar,
    Rol,
    Ror,
    UDiv,
    URem,
    SDiv,
    SRem,
    MulHiU,
    MulHiS,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum UnOp {
    Not,
    Neg,
    Bswap,
    Clz,
    Ctz,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum CastKind {
    Zx,
    Sx,
    Trunc,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum Pred {
    Eq,
    Ne,
    Ult,
    Ule,
    Ugt,
    Uge,
    Slt,
    Sle,
    Sgt,
    Sge,
}

/// x86 condition codes in encoding order
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Cond {
    O = 0,
    No = 1,
    B = 2,
    Ae = 3,
    E = 4,
    Ne = 5,
    Be = 6,
    A = 7,
    S = 8,
    Ns = 9,
    P = 10,
    Np = 11,
    L = 12,
    Ge = 13,
    Le = 14,
    G = 15,
}

pub const FL_CF: u8 = 1;
pub const FL_PF: u8 = 2;
pub const FL_AF: u8 = 4;
pub const FL_ZF: u8 = 8;
pub const FL_SF: u8 = 16;
pub const FL_OF: u8 = 32;
pub const FL_ALL: u8 = 63;

/// x86 EFLAGS bit position of each flag in FL_* order
pub const FL_POS: [u32; 6] = [0, 2, 4, 6, 7, 11];

impl Cond {
    pub fn from_u8(n: u8) -> Cond {
        use Cond::*;
        [O, No, B, Ae, E, Ne, Be, A, S, Ns, P, Np, L, Ge, Le, G][n as usize & 15]
    }
    pub fn negate(self) -> Cond {
        Cond::from_u8(self as u8 ^ 1)
    }
    /// flags read by this condition
    pub fn flags(self) -> u8 {
        use Cond::*;
        match self {
            O | No => FL_OF,
            B | Ae => FL_CF,
            E | Ne => FL_ZF,
            Be | A => FL_CF | FL_ZF,
            S | Ns => FL_SF,
            P | Np => FL_PF,
            L | Ge => FL_SF | FL_OF,
            Le | G => FL_ZF | FL_SF | FL_OF,
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum FlagKind {
    Add,
    Adc,
    Sub,
    Sbb,
    Logic,
    Inc,
    Dec,
    Shl,
    Shr,
    Sar,
    Rol,
    Ror,
    /// `r` holds EFLAGS-format bits (only bits in `mask` are defined)
    Raw,
}

#[derive(Clone, Debug)]
pub struct FlagDef {
    pub kind: FlagKind,
    pub ty: Ty,
    /// flags defined by this producer
    pub mask: u8,
    pub a: Opnd,
    pub b: Opnd,
    pub r: Opnd,
    /// carry-in (adc/sbb)
    pub cin: Opnd,
    /// shift/rotate count (already masked)
    pub cnt: Opnd,
    /// if true, the definition only takes effect when `cnt != 0` (variable count shifts)
    pub cond_cnt: bool,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum AtomicOp {
    Add,
    Sub,
    And,
    Or,
    Xor,
    Xchg,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum StrKind {
    Movs,
    Stos,
    Lods,
    Cmps,
    Scas,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum Rep {
    None,
    Rep,
    Repe,
    Repne,
}

#[derive(Clone, Debug, PartialEq)]
pub enum CallTarget {
    /// lifted guest function
    Direct(u32),
    /// HLE import thunk (index into the program's import table)
    Hle(u32),
    Indirect(Opnd),
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum FOp {
    Add,
    Sub,
    Mul,
    Div,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum FUn {
    Neg,
    Abs,
    Sqrt,
    Sin,
    Cos,
    Rndint,
    F2xm1,
    Tan,
}

/// Memory formats for x87 loads/stores
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum FMem {
    F32,
    F64,
    F80,
    I16,
    I32,
    I64,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub enum VLane {
    F32x4,
    F64x2,
    I8x16,
    I16x8,
    I32x4,
    I64x2,
}

#[derive(Clone, Debug)]
pub enum Op {
    Mov { ty: Ty, d: Place, a: Opnd },
    Bin { op: BinOp, ty: Ty, d: Place, a: Opnd, b: Opnd },
    Un { op: UnOp, ty: Ty, d: Place, a: Opnd },
    Cast { k: CastKind, from: Ty, to: Ty, d: Place, a: Opnd },
    Sel { ty: Ty, d: Place, c: Opnd, a: Opnd, b: Opnd },
    Cmp { pred: Pred, ty: Ty, d: Place, a: Opnd, b: Opnd },
    Lea { d: Place, m: Mem },
    Ld { ty: Ty, d: Place, m: Mem },
    St { ty: Ty, m: Mem, v: Opnd },
    /// Partial register write: reg = (reg & ~(mask<<shift)) | (v << shift)
    SetPart { r: Gpr, ty: Ty, shift: u8, v: Opnd },
    /// 32x32->64 (or 16x16->32, 8x8->16) widening multiply: (hi:lo) = a * b
    MulWide { signed: bool, ty: Ty, hi: Place, lo: Place, a: Opnd, b: Opnd },
    /// (hi:lo) / divisor -> q, r. Faults (trap) on zero divisor or overflow.
    DivWide { signed: bool, ty: Ty, q: Place, r: Place, hi: Opnd, lo: Opnd, div: Opnd },
    AtomicRmw { op: AtomicOp, ty: Ty, old: Place, m: Mem, v: Opnd },
    /// compare-and-swap; old = previous value; (flags are produced by a separate FlagDef on (expected, old))
    AtomicCas { ty: Ty, old: Place, m: Mem, expected: Opnd, new: Opnd },
    Fence,
    // ---- flags
    FlagDef(FlagDef),
    FlagGet { d: Place, cond: Cond },
    FlagGetBit { d: Place, flag: u8 },
    /// d (I32) = EFLAGS image (CF,PF,AF,ZF,SF,OF from lazy state, DF, plus fixed bits 1 and IF)
    FlagsPack { d: Place },
    SetDf(Opnd),
    GetDf { d: Place },
    // ---- control
    /// guest call; esp/eax/ecx/edx effects are applied by the emitter from the callee summary
    Call { target: CallTarget, ret_addr: u32 },
    /// d (an x87 slot) = guest_thread->st0_ret: the float returned by the call that just completed
    FRetLoad { d: Place },
    /// guest_thread->st0_ret = a (the function returns a float in st(0))
    FRetStore { a: Opnd },
    /// call of a native x87 helper `double host(st1, st0)` (n = 1 or 2 arguments popped, one result pushed)
    FHost { sym: u32, n: u8, d: Place, a: Opnd, b: Opnd },
    /// EH landing pad (after the frame is set up): registers the frame (ebp) for non-local resumption and sets d (I32) to 0;
    /// when execution is resumed at conts[i] (a catch / __except continuation of this function) control comes back here with
    /// ebp restored and d = i + 1. A Switch on d follows.
    ResumeSetup { d: Place, conts: Vec<u32> },
    // ---- string instructions (size in bytes: 1,2,4)
    Str { kind: StrKind, size: u8, rep: Rep, src_local: bool, dst_local: bool, ta: Place, tb: Place, done: Place },
    // ---- x87 (relative stack until resolved)
    FPush { v: Opnd },
    FPop,
    FXch { i: u8 },
    FMov { d: Place, a: Opnd },
    FBin { op: FOp, d: Place, a: Opnd, b: Opnd },
    FUn { op: FUn, d: Place, a: Opnd },
    FLdM { fmt: FMem, d: Place, m: Mem },
    FStM { fmt: FMem, m: Mem, a: Opnd, trunc: bool },
    FConst { d: Place, v: f64 },
    /// compare a,b -> sets fpu condition code (C0/C2/C3) state; `unordered_ok` for fucom
    FCom { a: Opnd, b: Opnd },
    /// fcomi family: sets ZF/PF/CF
    FComi { d: Place, a: Opnd, b: Opnd },
    FTst { a: Opnd },
    FXam { a: Opnd },
    FNstsw { d: Place, top: u8 },
    FNstcw { d: Place },
    FLdcw { v: Opnd },
    FCmov { c: Opnd, d: Place, a: Opnd },
    /// fprem/fprem1 (partial remainder): d = rem(d, b); sets C2=0, C0,C3,C1 from quotient bits
    FPrem { ieee: bool, d: Place, b: Opnd },
    FScale { d: Place, b: Opnd },
    FYl2x { d: Place, a: Opnd, b: Opnd },
    FAtan2 { d: Place, y: Opnd, x: Opnd },
    FInit,
    // ---- SSE / MMX (V128 values; MMX registers are modelled as V128 with don't-care upper half)
    /// generic vector function `lx_<f>(a, b, imm)` from the generated helper header; `ty` is the result type
    V { f: VFn, ty: Ty, d: Place, a: Opnd, b: Opnd, imm: u8 },
    /// load `w` bytes (4, 8, 16) into the low lane; if `zx` the rest of the register is zeroed, else it is preserved
    VLd { w: u8, zx: bool, d: Place, m: Mem },
    VSt { w: u8, m: Mem, a: Opnd },
    Misc(MiscOp),
    /// unsupported instruction: transfer to the runtime trap with this address
    Trap { why: u16 },
}

/// name of a vector helper (`lx_<name>`) in the generated gen.h
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct VFn(pub &'static str);

#[derive(Clone, Debug)]
pub enum MiscOp {
    Cpuid,
    Rdtsc,
    Pause,
    Nop,
    Prefetch(Mem),
    Emms,
    Ldmxcsr { v: Opnd },
    Stmxcsr { d: Place },
    Sfence,
    Lfence,
    Mfence,
}

#[derive(Clone, Debug)]
pub struct Inst {
    pub addr: u32,
    pub op: Op,
}

#[derive(Clone, Debug)]
pub enum Term {
    /// fall into the next block (by address)
    Fall(u32),
    Jmp(u32),
    Br { c: Opnd, t: u32, f: u32 },
    Switch { idx: Opnd, targets: Vec<u32> },
    Ret { pop: u16 },
    /// tail call to a guest address
    TailCall(CallTarget),
    /// function end after a call that never returns, or an undecodable instruction
    Unreachable,
    Trap { why: u16 },
}

#[derive(Clone, Debug)]
pub struct Block {
    pub addr: u32,
    pub insts: Vec<Inst>,
    pub term: Term,
    /// address of the terminating instruction
    pub term_addr: u32,
}

#[derive(Clone, Debug, Default)]
pub struct Func {
    pub entry: u32,
    pub blocks: Vec<Block>,
    pub tmp_ty: Vec<Ty>,
    pub n_x86: u32,
}

impl Func {
    pub fn new_tmp(&mut self, ty: Ty) -> Tmp {
        self.tmp_ty.push(ty);
        (self.tmp_ty.len() - 1) as Tmp
    }
}

impl fmt::Display for Gpr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.name())
    }
}
