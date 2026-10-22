//! C expressions for flag bits and fused conditions of each flag-producer kind.

use ir::*;

pub fn cty(ty: Ty) -> &'static str {
    match ty {
        Ty::I1 => "_Bool",
        Ty::I8 => "uint8_t",
        Ty::I16 => "uint16_t",
        Ty::I32 => "uint32_t",
        Ty::I64 => "uint64_t",
        Ty::F32 => "float",
        Ty::F64 => "double",
        Ty::V128 => "v128",
    }
}
pub fn sty(ty: Ty) -> &'static str {
    match ty {
        Ty::I8 => "int8_t",
        Ty::I16 => "int16_t",
        Ty::I32 => "int32_t",
        Ty::I64 => "int64_t",
        _ => "int32_t",
    }
}

pub struct V<'a> {
    pub a: &'a str,
    pub b: &'a str,
    pub r: &'a str,
    pub ci: &'a str,
    pub cn: &'a str,
}

fn sign_min(ty: Ty) -> String {
    match ty {
        Ty::I8 => "0x80u".into(),
        Ty::I16 => "0x8000u".into(),
        _ => "0x80000000u".into(),
    }
}
fn sign_max(ty: Ty) -> String {
    match ty {
        Ty::I8 => "0x7fu".into(),
        Ty::I16 => "0x7fffu".into(),
        _ => "0x7fffffffu".into(),
    }
}

/// Expression (C truth value) of flag bit `k` (index into CF,PF,AF,ZF,SF,OF) for a producer of `kind`/`ty`.
pub fn bit_formula(kind: FlagKind, ty: Ty, k: usize, v: &V) -> Option<String> {
    let t = cty(ty);
    let st = sty(ty);
    let bits = ty.bits();
    let (a, b, r, ci, cn) = (v.a, v.b, v.r, v.ci, v.cn);
    let pf = format!("(!__builtin_parity((uint32_t)(uint8_t){r}))");
    let zf = format!("({r} == 0)");
    let sf = format!("(({st}){r} < 0)");
    let af = format!("(((({a} ^ {b} ^ {r}) & 0x10u) != 0))");
    // CF of shl for narrow operands needs a guard
    let shl_cf = if bits == 32 {
        format!("((((uint32_t){a}) >> (32u - {cn})) & 1u)")
    } else {
        format!("(({cn} <= {bits}u) ? ((((uint32_t){a}) >> ({bits}u - {cn})) & 1u) : 0u)")
    };
    match kind {
        FlagKind::Add | FlagKind::Adc => Some(match k {
            0 => {
                if kind == FlagKind::Add {
                    format!("({r} < {a})")
                } else {
                    format!("({ci} ? ({r} <= {a}) : ({r} < {a}))")
                }
            }
            1 => pf,
            2 => af,
            3 => zf,
            4 => sf,
            _ => format!("(({st})(({a} ^ {r}) & ({b} ^ {r})) < 0)"),
        }),
        FlagKind::Sub | FlagKind::Sbb => Some(match k {
            0 => {
                if kind == FlagKind::Sub {
                    format!("({a} < {b})")
                } else {
                    format!("({ci} ? ({a} <= {b}) : ({a} < {b}))")
                }
            }
            1 => pf,
            2 => af,
            3 => zf,
            4 => sf,
            _ => format!("(({st})(({a} ^ {b}) & ({a} ^ {r})) < 0)"),
        }),
        FlagKind::Logic => Some(match k {
            0 | 2 | 5 => "0".into(),
            1 => pf,
            3 => zf,
            _ => sf,
        }),
        FlagKind::Inc => match k {
            0 => None,
            1 => Some(pf),
            2 => Some(format!("(((({a} ^ 1u ^ {r}) & 0x10u) != 0))")),
            3 => Some(zf),
            4 => Some(sf),
            _ => Some(format!("({r} == ({t}){})", sign_min(ty))),
        },
        FlagKind::Dec => match k {
            0 => None,
            1 => Some(pf),
            2 => Some(format!("(((({a} ^ 1u ^ {r}) & 0x10u) != 0))")),
            3 => Some(zf),
            4 => Some(sf),
            _ => Some(format!("({r} == ({t}){})", sign_max(ty))),
        },
        FlagKind::Shl => match k {
            0 => Some(format!("({shl_cf} != 0)")),
            1 => Some(pf),
            2 => None,
            3 => Some(zf),
            4 => Some(sf),
            _ => Some(format!("(((((uint32_t){r} >> {}u) & 1u) != 0) != ({shl_cf} != 0))", bits - 1)),
        },
        FlagKind::Shr => match k {
            0 => Some(format!("(((((uint32_t){a}) >> ({cn} - 1u)) & 1u) != 0)")),
            1 => Some(pf),
            2 => None,
            3 => Some(zf),
            4 => Some(sf),
            _ => Some(format!("(((((uint32_t)({a} ^ {r})) >> {}u) & 1u) != 0)", bits - 1)),
        },
        FlagKind::Sar => match k {
            0 => Some(format!("(((((int32_t)({st}){a}) >> ({cn} - 1u)) & 1) != 0)")),
            1 => Some(pf),
            2 => None,
            3 => Some(zf),
            4 => Some(sf),
            _ => Some("0".into()),
        },
        FlagKind::Rol => match k {
            0 => Some(format!("(({r} & 1u) != 0)")),
            5 => Some(format!("(((((uint32_t){r} >> {}u) ^ (uint32_t){r}) & 1u) != 0)", bits - 1)),
            _ => None,
        },
        FlagKind::Ror => match k {
            0 => Some(format!("((((uint32_t){r} >> {}u) & 1u) != 0)", bits - 1)),
            5 => Some(format!("(((((uint32_t){r} >> {}u) ^ ((uint32_t){r} >> {}u)) & 1u) != 0)", bits - 1, bits - 2)),
            _ => None,
        },
        FlagKind::Raw => Some(format!("((({r} >> {}u) & 1u) != 0)", FL_POS[k])),
    }
}

/// Fused single-comparison forms of a condition for a lazy class.
pub fn fused_cond(kind: FlagKind, ty: Ty, c: Cond, v: &V) -> Option<String> {
    let st = sty(ty);
    let (a, b, r) = (v.a, v.b, v.r);
    use Cond::*;
    match kind {
        FlagKind::Sub => Some(match c {
            E => format!("({a} == {b})"),
            Ne => format!("({a} != {b})"),
            B => format!("({a} < {b})"),
            Ae => format!("({a} >= {b})"),
            Be => format!("({a} <= {b})"),
            A => format!("({a} > {b})"),
            L => format!("(({st}){a} < ({st}){b})"),
            Ge => format!("(({st}){a} >= ({st}){b})"),
            Le => format!("(({st}){a} <= ({st}){b})"),
            G => format!("(({st}){a} > ({st}){b})"),
            S => format!("(({st}){r} < 0)"),
            Ns => format!("(({st}){r} >= 0)"),
            _ => return None,
        }),
        FlagKind::Logic => Some(match c {
            E => format!("({r} == 0)"),
            Ne => format!("({r} != 0)"),
            S | L => format!("(({st}){r} < 0)"),
            Ns | Ge => format!("(({st}){r} >= 0)"),
            B | O => "0".into(),
            Ae | No => "1".into(),
            Be => format!("({r} == 0)"),
            A => format!("({r} != 0)"),
            Le => format!("(({st}){r} <= 0)"),
            G => format!("(({st}){r} > 0)"),
            _ => return None,
        }),
        FlagKind::Add => Some(match c {
            E => format!("({r} == 0)"),
            Ne => format!("({r} != 0)"),
            B => format!("({r} < {a})"),
            Ae => format!("({r} >= {a})"),
            S => format!("(({st}){r} < 0)"),
            Ns => format!("(({st}){r} >= 0)"),
            _ => return None,
        }),
        FlagKind::Inc | FlagKind::Dec | FlagKind::Shl | FlagKind::Shr | FlagKind::Sar => Some(match c {
            E => format!("({r} == 0)"),
            Ne => format!("({r} != 0)"),
            S => format!("(({st}){r} < 0)"),
            Ns => format!("(({st}){r} >= 0)"),
            _ => return None,
        }),
        _ => None,
    }
}

/// Combine individual flag bit expressions into a condition.
pub fn combine(c: Cond, bit: &dyn Fn(usize) -> String) -> String {
    use Cond::*;
    let cf = || bit(0);
    let pf = || bit(1);
    let zf = || bit(3);
    let sf = || bit(4);
    let of = || bit(5);
    match c {
        O => of(),
        No => format!("(!{})", of()),
        B => cf(),
        Ae => format!("(!{})", cf()),
        E => zf(),
        Ne => format!("(!{})", zf()),
        Be => format!("({} || {})", cf(), zf()),
        A => format!("(!({} || {}))", cf(), zf()),
        S => sf(),
        Ns => format!("(!{})", sf()),
        P => pf(),
        Np => format!("(!{})", pf()),
        L => format!("({} != {})", sf(), of()),
        Ge => format!("({} == {})", sf(), of()),
        Le => format!("({} || ({} != {}))", zf(), sf(), of()),
        G => format!("((!{}) && ({} == {}))", zf(), sf(), of()),
    }
}
