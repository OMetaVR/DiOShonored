//! SSE / SSE2 / MMX lowering. Almost every instruction has the shape `dst = lx_<f>(dst_old, src[, imm])` on 128-bit values;
//! the semantics of each `f` live in the generated helper header (one place for vector semantics, portable across
//! SSE and NEON). MMX registers are modelled as the low half of extra vector registers.

use crate::lower::*;
use crate::repr::*;
use iced_x86::{Instruction, Mnemonic, OpKind, Register};

fn vreg(r: Register) -> Option<Place> {
    use Register::*;
    Some(match r {
        XMM0 => Place::Xmm(0),
        XMM1 => Place::Xmm(1),
        XMM2 => Place::Xmm(2),
        XMM3 => Place::Xmm(3),
        XMM4 => Place::Xmm(4),
        XMM5 => Place::Xmm(5),
        XMM6 => Place::Xmm(6),
        XMM7 => Place::Xmm(7),
        MM0 => Place::Mm(0),
        MM1 => Place::Mm(1),
        MM2 => Place::Mm(2),
        MM3 => Place::Mm(3),
        MM4 => Place::Mm(4),
        MM5 => Place::Mm(5),
        MM6 => Place::Mm(6),
        MM7 => Place::Mm(7),
        _ => return Option::None,
    })
}

enum K {
    /// d = f(d, src)
    Bin(&'static str),
    /// d = f(d, src, imm8)
    BinImm(&'static str),
    /// shift: imm form uses f_s with an integer count, register form f_v
    Shift(&'static str),
    /// result is an integer (GPR destination): r = f(src[, imm])
    ToInt(&'static str, bool),
    /// source is an integer (GPR/mem operand): d = f(d, int[, imm])
    FromInt(&'static str, bool),
}

fn classify(m: Mnemonic) -> Option<K> {
    use Mnemonic as M;
    Some(match m {
        M::Addps => K::Bin("addps"),
        M::Addss => K::Bin("addss"),
        M::Addpd => K::Bin("addpd"),
        M::Addsd => K::Bin("addsd"),
        M::Subps => K::Bin("subps"),
        M::Subss => K::Bin("subss"),
        M::Subpd => K::Bin("subpd"),
        M::Subsd => K::Bin("subsd"),
        M::Mulps => K::Bin("mulps"),
        M::Mulss => K::Bin("mulss"),
        M::Mulpd => K::Bin("mulpd"),
        M::Mulsd => K::Bin("mulsd"),
        M::Divps => K::Bin("divps"),
        M::Divss => K::Bin("divss"),
        M::Divpd => K::Bin("divpd"),
        M::Divsd => K::Bin("divsd"),
        M::Minps => K::Bin("minps"),
        M::Minss => K::Bin("minss"),
        M::Minpd => K::Bin("minpd"),
        M::Minsd => K::Bin("minsd"),
        M::Maxps => K::Bin("maxps"),
        M::Maxss => K::Bin("maxss"),
        M::Maxpd => K::Bin("maxpd"),
        M::Maxsd => K::Bin("maxsd"),
        M::Sqrtps => K::Bin("sqrtps"),
        M::Sqrtss => K::Bin("sqrtss"),
        M::Sqrtpd => K::Bin("sqrtpd"),
        M::Sqrtsd => K::Bin("sqrtsd"),
        M::Rcpps => K::Bin("rcpps"),
        M::Rcpss => K::Bin("rcpss"),
        M::Rsqrtps => K::Bin("rsqrtps"),
        M::Rsqrtss => K::Bin("rsqrtss"),
        M::Andps | M::Andpd | M::Pand => K::Bin("and"),
        M::Andnps | M::Andnpd | M::Pandn => K::Bin("andn"),
        M::Orps | M::Orpd | M::Por => K::Bin("or"),
        M::Xorps | M::Xorpd | M::Pxor => K::Bin("xor"),
        M::Cmpps => K::BinImm("cmpps_i"),
        M::Cmpss => K::BinImm("cmpss_i"),
        M::Cmppd => K::BinImm("cmppd_i"),
        M::Cmpsd => K::BinImm("cmpsd_i"),
        M::Pcmpeqb => K::Bin("pcmpeqb"),
        M::Pcmpeqw => K::Bin("pcmpeqw"),
        M::Pcmpeqd => K::Bin("pcmpeqd"),
        M::Pcmpgtb => K::Bin("pcmpgtb"),
        M::Pcmpgtw => K::Bin("pcmpgtw"),
        M::Pcmpgtd => K::Bin("pcmpgtd"),
        M::Paddb => K::Bin("paddb"),
        M::Paddw => K::Bin("paddw"),
        M::Paddd => K::Bin("paddd"),
        M::Paddq => K::Bin("paddq"),
        M::Psubb => K::Bin("psubb"),
        M::Psubw => K::Bin("psubw"),
        M::Psubd => K::Bin("psubd"),
        M::Psubq => K::Bin("psubq"),
        M::Paddsb => K::Bin("paddsb"),
        M::Paddsw => K::Bin("paddsw"),
        M::Paddusb => K::Bin("paddusb"),
        M::Paddusw => K::Bin("paddusw"),
        M::Psubsb => K::Bin("psubsb"),
        M::Psubsw => K::Bin("psubsw"),
        M::Psubusb => K::Bin("psubusb"),
        M::Psubusw => K::Bin("psubusw"),
        M::Pmullw => K::Bin("pmullw"),
        M::Pmulhw => K::Bin("pmulhw"),
        M::Pmulhuw => K::Bin("pmulhuw"),
        M::Pmuludq => K::Bin("pmuludq"),
        M::Pmaddwd => K::Bin("pmaddwd"),
        M::Pmaxsw => K::Bin("pmaxsw"),
        M::Pminsw => K::Bin("pminsw"),
        M::Pmaxub => K::Bin("pmaxub"),
        M::Pminub => K::Bin("pminub"),
        M::Pavgb => K::Bin("pavgb"),
        M::Pavgw => K::Bin("pavgw"),
        M::Psadbw => K::Bin("psadbw"),
        M::Unpcklps => K::Bin("unpcklps"),
        M::Unpckhps => K::Bin("unpckhps"),
        M::Unpcklpd => K::Bin("unpcklpd"),
        M::Unpckhpd => K::Bin("unpckhpd"),
        M::Punpcklbw => K::Bin("punpcklbw"),
        M::Punpcklwd => K::Bin("punpcklwd"),
        M::Punpckldq => K::Bin("punpckldq"),
        M::Punpcklqdq => K::Bin("punpcklqdq"),
        M::Punpckhbw => K::Bin("punpckhbw"),
        M::Punpckhwd => K::Bin("punpckhwd"),
        M::Punpckhdq => K::Bin("punpckhdq"),
        M::Punpckhqdq => K::Bin("punpckhqdq"),
        M::Packssdw => K::Bin("packssdw"),
        M::Packsswb => K::Bin("packsswb"),
        M::Packuswb => K::Bin("packuswb"),
        M::Cvtss2sd => K::Bin("cvtss2sd"),
        M::Cvtsd2ss => K::Bin("cvtsd2ss"),
        M::Cvtdq2ps => K::Bin("cvtdq2ps"),
        M::Cvtps2dq => K::Bin("cvtps2dq"),
        M::Cvttps2dq => K::Bin("cvttps2dq"),
        M::Cvtdq2pd => K::Bin("cvtdq2pd"),
        M::Cvtpd2ps => K::Bin("cvtpd2ps"),
        M::Cvtps2pd => K::Bin("cvtps2pd"),
        M::Cvtpd2dq => K::Bin("cvtpd2dq"),
        M::Cvttpd2dq => K::Bin("cvttpd2dq"),
        M::Cvtpi2ps => K::Bin("cvtpi2ps"),
        M::Cvtps2pi => K::Bin("cvtps2pi"),
        M::Cvttps2pi => K::Bin("cvttps2pi"),
        M::Movlhps => K::Bin("movlhps"),
        M::Movhlps => K::Bin("movhlps"),
        M::Shufps => K::BinImm("shufps_i"),
        M::Shufpd => K::BinImm("shufpd_i"),
        M::Pshufd => K::BinImm("pshufd_i"),
        M::Pshuflw => K::BinImm("pshuflw_i"),
        M::Pshufhw => K::BinImm("pshufhw_i"),
        M::Pshufw => K::BinImm("pshufw_i"),
        M::Psllw => K::Shift("psllw"),
        M::Pslld => K::Shift("pslld"),
        M::Psllq => K::Shift("psllq"),
        M::Psrlw => K::Shift("psrlw"),
        M::Psrld => K::Shift("psrld"),
        M::Psrlq => K::Shift("psrlq"),
        M::Psraw => K::Shift("psraw"),
        M::Psrad => K::Shift("psrad"),
        M::Movmskps => K::ToInt("movmskps", false),
        M::Movmskpd => K::ToInt("movmskpd", false),
        M::Pmovmskb => K::ToInt("pmovmskb", false),
        M::Cvttss2si => K::ToInt("cvttss2si", false),
        M::Cvtss2si => K::ToInt("cvtss2si", false),
        M::Cvttsd2si => K::ToInt("cvttsd2si", false),
        M::Cvtsd2si => K::ToInt("cvtsd2si", false),
        M::Pextrw => K::ToInt("pextrw_i", true),
        M::Cvtsi2ss => K::FromInt("cvtsi2ss", false),
        M::Cvtsi2sd => K::FromInt("cvtsi2sd", false),
        M::Pinsrw => K::FromInt("pinsrw_i", true),
        _ => return Option::None,
    })
}

impl<'a> Lower<'a> {
    fn vsrc(&mut self, i: &Instruction, n: u32) -> Option<Opnd> {
        match i.op_kind(n) {
            OpKind::Register => vreg(i.op_register(n)).map(Opnd::P),
            OpKind::Memory => {
                let w = i.memory_size().size() as u8;
                let w = if matches!(w, 4 | 8 | 16) { w } else { 16 };
                let m = self.mem(i);
                let d = self.tmp(Ty::V128);
                self.emit(Op::VLd { w, zx: true, d, m });
                Some(Opnd::P(d))
            }
            _ => Option::None,
        }
    }

    pub fn lower_sse(&mut self, i: &Instruction) -> bool {
        use Mnemonic as M;
        let m = i.mnemonic();
        // ---- moves
        match m {
            M::Movaps | M::Movups | M::Movdqa | M::Movdqu | M::Movapd | M::Movupd | M::Lddqu | M::Movntps | M::Movntdq | M::Movntpd => {
                if i.op0_kind() == OpKind::Memory {
                    let src = match vreg(i.op1_register()) {
                        Some(p) => Opnd::P(p),
                        None => {
                            self.unsupported("sse store src");
                            return true;
                        }
                    };
                    let mm = self.mem(i);
                    self.emit(Op::VSt { w: 16, m: mm, a: src });
                } else if let (Some(d), Some(s)) = (vreg(i.op0_register()), self.vsrc(i, 1)) {
                    self.emit(Op::Mov { ty: Ty::V128, d, a: s });
                } else {
                    self.unsupported("movaps form");
                }
                return true;
            }
            M::Movntq => {
                let src = vreg(i.op1_register()).map(Opnd::P);
                if let Some(s) = src {
                    let mm = self.mem(i);
                    self.emit(Op::VSt { w: 8, m: mm, a: s });
                } else {
                    self.unsupported("movntq");
                }
                return true;
            }
            M::Movss | M::Movsd if i.op_count() == 2 && !(m == M::Movsd && is_string(i)) => {
                let w: u8 = if m == M::Movss { 4 } else { 8 };
                if i.op0_kind() == OpKind::Memory {
                    let s = vreg(i.op1_register()).map(Opnd::P);
                    let mm = self.mem(i);
                    if let Some(s) = s {
                        self.emit(Op::VSt { w, m: mm, a: s });
                    }
                } else if i.op1_kind() == OpKind::Memory {
                    let mm = self.mem(i);
                    if let Some(d) = vreg(i.op0_register()) {
                        self.emit(Op::VLd { w, zx: true, d, m: mm });
                    }
                } else if let (Some(d), Some(s)) = (vreg(i.op0_register()), vreg(i.op1_register())) {
                    let f = if m == M::Movss { "movss_rr" } else { "movsd_rr" };
                    self.emit(Op::V { f: VFn(f), ty: Ty::V128, d, a: Opnd::P(d), b: Opnd::P(s), imm: 0 });
                }
                return true;
            }
            M::Movq if i.op0_kind() == OpKind::Register && vreg(i.op0_register()).is_some() || m == M::Movq && i.op0_kind() == OpKind::Memory => {
                if i.op0_kind() == OpKind::Memory {
                    let mm = self.mem(i);
                    if let Some(s) = vreg(i.op1_register()) {
                        self.emit(Op::VSt { w: 8, m: mm, a: Opnd::P(s) });
                    }
                } else if i.op1_kind() == OpKind::Memory {
                    let mm = self.mem(i);
                    self.emit(Op::VLd { w: 8, zx: true, d: vreg(i.op0_register()).unwrap(), m: mm });
                } else if let (Some(d), Some(s)) = (vreg(i.op0_register()), vreg(i.op1_register())) {
                    self.emit(Op::V { f: VFn("movq_rr"), ty: Ty::V128, d, a: Opnd::P(d), b: Opnd::P(s), imm: 0 });
                } else {
                    self.unsupported("movq form");
                }
                return true;
            }
            M::Movd if i.op0_kind() == OpKind::Register && vreg(i.op0_register()).is_some() => {
                let d = vreg(i.op0_register()).unwrap();
                let v = self.read_op(i, 1, Ty::I32);
                self.emit(Op::V { f: VFn("vfrom32"), ty: Ty::V128, d, a: Opnd::Imm(0), b: v, imm: 0 });
                return true;
            }
            M::Movd => {
                // movd r/m32, xmm/mm
                if let Some(s) = vreg(i.op1_register()) {
                    let t = self.tmp(Ty::I32);
                    self.emit(Op::V { f: VFn("vto32"), ty: Ty::I32, d: t, a: Opnd::Imm(0), b: Opnd::P(s), imm: 0 });
                    self.write_op(i, 0, Ty::I32, Opnd::P(t));
                } else {
                    self.unsupported("movd form");
                }
                return true;
            }
            M::Movlps | M::Movlpd | M::Movhps | M::Movhpd => {
                let hi = matches!(m, M::Movhps | M::Movhpd);
                if i.op0_kind() == OpKind::Memory {
                    let s = vreg(i.op1_register()).unwrap();
                    let mm = self.mem(i);
                    if hi {
                        let t = self.tmp(Ty::V128);
                        self.emit(Op::V { f: VFn("movhi_to_lo"), ty: Ty::V128, d: t, a: Opnd::Imm(0), b: Opnd::P(s), imm: 0 });
                        self.emit(Op::VSt { w: 8, m: mm, a: Opnd::P(t) });
                    } else {
                        self.emit(Op::VSt { w: 8, m: mm, a: Opnd::P(s) });
                    }
                } else {
                    let d = vreg(i.op0_register()).unwrap();
                    let mm = self.mem(i);
                    if hi {
                        let t = self.tmp(Ty::V128);
                        self.emit(Op::VLd { w: 8, zx: true, d: t, m: mm });
                        self.emit(Op::V { f: VFn("movhi_ld"), ty: Ty::V128, d, a: Opnd::P(d), b: Opnd::P(t), imm: 0 });
                    } else {
                        self.emit(Op::VLd { w: 8, zx: false, d, m: mm });
                    }
                }
                return true;
            }
            M::Comiss | M::Ucomiss | M::Comisd | M::Ucomisd => {
                let a = match vreg(i.op0_register()) {
                    Some(p) => Opnd::P(p),
                    None => return true,
                };
                let b = match self.vsrc(i, 1) {
                    Some(b) => b,
                    None => return true,
                };
                let t = self.tmp(Ty::I32);
                let f = if matches!(m, M::Comiss | M::Ucomiss) { "comiss" } else { "comisd" };
                self.emit(Op::V { f: VFn(f), ty: Ty::I32, d: t, a, b, imm: 0 });
                self.emit(Op::FlagDef(FlagDef { kind: FlagKind::Raw, ty: Ty::I32, mask: FL_ALL, a: Opnd::Imm(0), b: Opnd::Imm(0), r: Opnd::P(t), cin: Opnd::Imm(0), cnt: Opnd::Imm(0), cond_cnt: false }));
                return true;
            }
            M::Psrldq | M::Pslldq => {
                let d = vreg(i.op0_register()).unwrap();
                let imm = i.immediate(1) as u8;
                let f = if m == M::Psrldq { "psrldq_i" } else { "pslldq_i" };
                self.emit(Op::V { f: VFn(f), ty: Ty::V128, d, a: Opnd::P(d), b: Opnd::Imm(0), imm });
                return true;
            }
            M::Ldmxcsr => {
                let v = self.read_op(i, 0, Ty::I32);
                self.emit(Op::Misc(MiscOp::Ldmxcsr { v }));
                return true;
            }
            M::Stmxcsr => {
                let t = self.tmp(Ty::I32);
                self.emit(Op::Misc(MiscOp::Stmxcsr { d: t }));
                let mm = self.mem(i);
                self.emit(Op::St { ty: Ty::I32, m: mm, v: Opnd::P(t) });
                return true;
            }
            _ => {}
        }
        let k = match classify(m) {
            Some(k) => k,
            None => return false,
        };
        match k {
            K::Bin(f) | K::BinImm(f) => {
                let d = match vreg(i.op0_register()) {
                    Some(d) => d,
                    None => {
                        self.unsupported("sse dest");
                        return true;
                    }
                };
                let b = match self.vsrc(i, 1) {
                    Some(b) => b,
                    None => {
                        self.unsupported("sse src");
                        return true;
                    }
                };
                let imm = if matches!(k, K::BinImm(_)) { i.immediate(2) as u8 } else { 0 };
                let f = if matches!(d, Place::Mm(_)) { mm_name(f) } else { f };
                self.emit(Op::V { f: VFn(f), ty: Ty::V128, d, a: Opnd::P(d), b, imm });
            }
            K::Shift(f) => {
                let d = vreg(i.op0_register()).unwrap();
                let (name, b): (&'static str, Opnd) = if matches!(i.op_kind(1), OpKind::Immediate8) {
                    (shift_name(f, true), Opnd::Imm(i.immediate(1) & 0xff))
                } else {
                    let b = self.vsrc(i, 1).unwrap();
                    (shift_name(f, false), b)
                };
                self.emit(Op::V { f: VFn(name), ty: Ty::V128, d, a: Opnd::P(d), b, imm: 0 });
            }
            K::ToInt(f, has_imm) => {
                let src = match self.vsrc(i, 1) {
                    Some(s) => s,
                    None => {
                        self.unsupported("sse int src");
                        return true;
                    }
                };
                let imm = if has_imm { i.immediate(2) as u8 } else { 0 };
                let f = if matches!(src, Opnd::P(Place::Mm(_))) { mm_name(f) } else { f };
                let t = self.tmp(Ty::I32);
                self.emit(Op::V { f: VFn(f), ty: Ty::I32, d: t, a: Opnd::Imm(0), b: src, imm });
                self.write_reg(i.op0_register(), Opnd::P(t));
            }
            K::FromInt(f, has_imm) => {
                let d = vreg(i.op0_register()).unwrap();
                let ity = if has_imm { Ty::I16 } else { Ty::I32 };
                let v = self.read_op(i, 1, ity);
                let v = self.cast(CastKind::Zx, ity, Ty::I32, v);
                let imm = if has_imm { i.immediate(2) as u8 } else { 0 };
                self.emit(Op::V { f: VFn(f), ty: Ty::V128, d, a: Opnd::P(d), b: v, imm });
            }
        }
        true
    }
}

fn shift_name(f: &'static str, imm: bool) -> &'static str {
    match (f, imm) {
        ("psllw", true) => "psllw_s",
        ("pslld", true) => "pslld_s",
        ("psllq", true) => "psllq_s",
        ("psrlw", true) => "psrlw_s",
        ("psrld", true) => "psrld_s",
        ("psrlq", true) => "psrlq_s",
        ("psraw", true) => "psraw_s",
        ("psrad", true) => "psrad_s",
        ("psllw", false) => "psllw_v",
        ("pslld", false) => "pslld_v",
        ("psllq", false) => "psllq_v",
        ("psrlw", false) => "psrlw_v",
        ("psrld", false) => "psrld_v",
        ("psrlq", false) => "psrlq_v",
        ("psraw", false) => "psraw_v",
        _ => "psrad_v",
    }
}

/// 64-bit MMX variants of the operations whose lane layout differs from the 128-bit form
fn mm_name(f: &'static str) -> &'static str {
    match f {
        "packssdw" => "packssdw_mm",
        "packsswb" => "packsswb_mm",
        "packuswb" => "packuswb_mm",
        "punpckhbw" => "punpckhbw_mm",
        "punpckhwd" => "punpckhwd_mm",
        "punpckhdq" => "punpckhdq_mm",
        "pmovmskb" => "pmovmskb_mm",
        other => other,
    }
}
