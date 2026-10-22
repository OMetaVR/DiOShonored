//! Calls, string ops, terminators and whole-function assembly for the C emitter.

use crate::ctx::OvrKind;
use crate::emit::*;
use ir::*;
use std::collections::BTreeMap;
use std::fmt::Write;

pub fn thunk_addr(i: u32) -> u32 {
    0xFFF0_0000u32 + i * 16
}

impl<'a> Emitter<'a> {
    fn ret_store(&mut self, ret_addr: u32) {
        self.line(&format!("GUEST_ST_LOCAL(uint32_t, esp - 4u, {:#x}u);", ret_addr));
    }

    pub fn emit_call(&mut self, target: &CallTarget, ret_addr: u32) {
        match target {
            CallTarget::Direct(a) => {
                if let Some((host, kind)) = self.callees.ovr(*a) {
                    if kind == OvrKind::GuestFn {
                        self.ovr_refs.insert(host.clone());
                        match self.callees.ovr_pop(*a) {
                            Some(p) => self.line(&format!("LX_CALLD({host}, {p}u);")),
                            None => self.line(&format!("LX_CALLD_DYN({host});")),
                        }
                        return;
                    }
                }
                let sig = self.callees.sig(*a);
                if sig.ret_addr {
                    self.ret_store(ret_addr);
                }
                if sig.ext_in == 0 && sig.ext_out == 0 {
                    self.refs.insert((*a, false));
                    match sig.pop {
                        Some(p) => self.line(&format!("LX_CALLD({}, {p}u);", gname(*a))),
                        None => self.line(&format!("LX_CALLD_DYN({});", gname(*a))),
                    }
                } else {
                    self.refs.insert((*a, true));
                    let mut args = String::from("esp - 4u, eax, ecx, edx");
                    for g in EXT_ORDER {
                        if sig.ext_in & ext_bit(g) != 0 {
                            args.push_str(&format!(", {}", g.name()));
                        }
                    }
                    let pop = match sig.pop {
                        Some(p) => format!("{p}u"),
                        None => "(uint32_t)(e_.r.r0 >> 32)".to_string(),
                    };
                    let mut s = format!("{{ LxExt e_ = {}_x({args}); eax = (uint32_t)e_.r.r0; edx = (uint32_t)e_.r.r1; esp += {pop};", gname(*a));
                    for (i, g) in EXT_ORDER.iter().enumerate() {
                        if sig.ext_out & ext_bit(*g) != 0 {
                            let _ = write!(s, " {} = e_.x[{i}];", g.name());
                        }
                    }
                    if sig.ext_out & 16 != 0 {
                        s.push_str(" ecx = e_.x[4];");
                    }
                    s.push_str(" }");
                    self.line(&s);
                }
            }
            CallTarget::Hle(i) => {
                self.line(&format!("LX_CALLI({:#x}u);", thunk_addr(*i)));
            }
            CallTarget::Indirect(o) => {
                let t = self.opnd(o, Ty::I32);
                self.line(&format!("{{ uint32_t ct_ = {t};"));
                self.ret_store(ret_addr);
                self.pass_ext_regs();
                self.line("LX_CALLI(ct_); }");
            }
        }
    }

    /// Before an indirect call: hand the callee-saved registers this function holds to whatever the call reaches
    /// (GuestThread.ext_regs, read by extended-signature functions in their generic entry). Registers it never touches still hold
    /// what its own caller left there.
    fn pass_ext_regs(&mut self) {
        let mut pass = String::new();
        for (i, g) in EXT_ORDER.iter().enumerate() {
            if self.gpr_used[*g as usize] || self.me.ext_in & ext_bit(*g) != 0 {
                let _ = write!(pass, "gt->ext_regs[{i}] = {}; ", g.name());
            }
        }
        if !pass.is_empty() {
            self.line(pass.trim_end());
        }
    }

    pub fn emit_tail(&mut self, target: &CallTarget, addr: u32) {
        let _ = addr;
        let me_ext = self.me.ext_in != 0 || self.me.ext_out != 0;
        match target {
            CallTarget::Direct(a) => {
                if let Some((host, kind)) = self.callees.ovr(*a) {
                    if kind == OvrKind::GuestFn {
                        self.ovr_refs.insert(host.clone());
                        let r = format!("{host}(esp, eax, ecx, edx)");
                        let s = self.wrap_ret(&r);
                        self.line(&s);
                        return;
                    }
                }
                let sig = self.callees.sig(*a);
                if sig.ext_in == 0 && sig.ext_out == 0 {
                    self.refs.insert((*a, false));
                    let r = format!("{}(esp, eax, ecx, edx)", gname(*a));
                    let s = self.wrap_ret(&r);
                    self.line(&s);
                } else {
                    self.refs.insert((*a, true));
                    let mut args = String::from("esp, eax, ecx, edx");
                    for g in EXT_ORDER {
                        if sig.ext_in & ext_bit(g) != 0 {
                            args.push_str(&format!(", {}", g.name()));
                        }
                    }
                    if !me_ext {
                        // callee extended outputs are dropped (the generic wrapper contract)
                        self.line(&format!("{{ LxExt e_ = {}_x({args}); return e_.r; }}", gname(*a)));
                    } else {
                        let mut s = format!("{{ LxExt c_ = {}_x({args}); LxExt e_; e_.r = c_.r;", gname(*a));
                        for (i, g) in EXT_ORDER.iter().enumerate() {
                            if sig.ext_out & ext_bit(*g) != 0 {
                                let _ = write!(s, " e_.x[{i}] = c_.x[{i}];");
                            } else if self.me.ext_out & ext_bit(*g) != 0 {
                                let _ = write!(s, " e_.x[{i}] = {};", g.name());
                            } else {
                                let _ = write!(s, " e_.x[{i}] = 0;");
                            }
                        }
                        if sig.ext_out & 16 != 0 {
                            s.push_str(" e_.x[4] = c_.x[4];");
                        } else if self.me.ext_out & 16 != 0 {
                            s.push_str(" e_.x[4] = ecx;");
                        } else {
                            s.push_str(" e_.x[4] = 0;");
                        }
                        s.push_str(" return e_; }");
                        self.line(&s);
                    }
                }
            }
            CallTarget::Hle(i) => {
                let r = format!("guest_call({:#x}u, esp, eax, ecx, edx)", thunk_addr(*i));
                let s = self.wrap_ret(&r);
                self.line(&s);
            }
            CallTarget::Indirect(o) => {
                let t = self.opnd(o, Ty::I32);
                self.pass_ext_regs();
                let r = format!("guest_call({t}, esp, eax, ecx, edx)");
                let s = self.wrap_ret(&r);
                self.line(&s);
            }
        }
    }

    pub fn emit_str(&mut self, op: &Op) {
        let (kind, size, rep, ta, tb, done) = match op {
            Op::Str { kind, size, rep, ta, tb, done, .. } => (*kind, *size, *rep, *ta, *tb, *done),
            _ => unreachable!(),
        };
        let t = match size {
            1 => "uint8_t",
            2 => "uint16_t",
            _ => "uint32_t",
        };
        let st = if self.uses_df_dyn { format!("(df ? (uint32_t)-{size} : {size}u)") } else { format!("{size}u") };
        let ld = |r: &str| format!("GUEST_LD_LOCAL({t}, {r})");
        let isrep = rep != Rep::None;
        let (tas, tbs, ds) = (self.place(&ta), self.place(&tb), self.place(&done));
        let mut s = String::new();
        match kind {
            StrKind::Movs => {
                if isrep {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; while (ecx) {{ {t} v_ = {}; GUEST_ST_LOCAL({t}, edi, v_); esi += st_; edi += st_; ecx--; }} }}", ld("esi"));
                } else {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {t} v_ = {}; GUEST_ST_LOCAL({t}, edi, v_); esi += st_; edi += st_; }}", ld("esi"));
                }
            }
            StrKind::Stos => {
                if isrep {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {t} v_ = ({t})eax; while (ecx) {{ GUEST_ST_LOCAL({t}, edi, v_); edi += st_; ecx--; }} }}");
                } else {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; GUEST_ST_LOCAL({t}, edi, ({t})eax); edi += st_; }}");
                }
            }
            StrKind::Lods => {
                let mask = if size == 4 { 0u32 } else if size == 2 { 0xffff0000 } else { 0xffffff00 };
                let upd = if size == 4 { format!("eax = {}", ld("esi")) } else { format!("eax = (eax & {mask:#x}u) | {}", ld("esi")) };
                if isrep {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; while (ecx) {{ {upd}; esi += st_; ecx--; }} }}");
                } else {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {upd}; esi += st_; }}");
                }
            }
            StrKind::Scas => {
                if isrep {
                    let brk = if rep == Rep::Repe { format!("if ({tas} != {tbs}) break;") } else { format!("if ({tas} == {tbs}) break;") };
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {ds} = 0; {tas} = ({t})eax; while (ecx) {{ {tbs} = {}; edi += st_; ecx--; {ds} = 1; {brk} }} }}", ld("edi"));
                } else {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {tas} = ({t})eax; {tbs} = {}; edi += st_; {ds} = 1; }}", ld("edi"));
                }
            }
            StrKind::Cmps => {
                if isrep {
                    let brk = if rep == Rep::Repe { format!("if ({tas} != {tbs}) break;") } else { format!("if ({tas} == {tbs}) break;") };
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {ds} = 0; while (ecx) {{ {tas} = {}; {tbs} = {}; esi += st_; edi += st_; ecx--; {ds} = 1; {brk} }} }}", ld("esi"), ld("edi"));
                } else {
                    let _ = write!(s, "{{ uint32_t st_ = {st}; {tas} = {}; {tbs} = {}; esi += st_; edi += st_; {ds} = 1; }}", ld("esi"), ld("edi"));
                }
            }
        }
        self.line(&s);
    }

    pub fn emit_misc(&mut self, m: &MiscOp) {
        match m {
            MiscOp::Cpuid => self.line("lx_cpuid(&eax, &ebx, &ecx, &edx);"),
            MiscOp::Rdtsc => self.line("lx_rdtsc(&eax, &edx);"),
            MiscOp::Pause | MiscOp::Nop | MiscOp::Emms => {}
            MiscOp::Prefetch(mm) => {
                let a = self.addr(mm);
                self.line(&format!("__builtin_prefetch(G({a}));"));
            }
            MiscOp::Sfence | MiscOp::Lfence | MiscOp::Mfence => self.line("__atomic_thread_fence(__ATOMIC_SEQ_CST);"),
            MiscOp::Ldmxcsr { v } => {
                let x = self.opnd(v, Ty::I32);
                self.line(&format!("gt->mxcsr = {x};"));
            }
            MiscOp::Stmxcsr { d } => {
                let dn = self.place(d);
                self.line(&format!("{dn} = gt->mxcsr;"));
            }
        }
    }

    fn emit_term(&mut self, bi: usize, b: &Block) {
        let next = bi as u32 + 1;
        let goto = |t: u32| format!("goto L{t};");
        match &b.term {
            Term::Fall(t) | Term::Jmp(t) => {
                if *t != next {
                    self.line(&goto(*t));
                }
            }
            Term::Br { c, t, f } => {
                let cc = self.opnd(c, Ty::I1);
                if *f == next {
                    self.line(&format!("if ({cc}) goto L{t};"));
                } else if *t == next {
                    self.line(&format!("if (!{cc}) goto L{f};"));
                } else {
                    self.line(&format!("if ({cc}) goto L{t};"));
                    self.line(&goto(*f));
                }
            }
            Term::Switch { idx, targets } => {
                let i = self.opnd(idx, Ty::I32);
                let mut s = format!("switch ({i}) {{");
                for (k, t) in targets.iter().enumerate() {
                    let _ = write!(s, " case {k}: goto L{t};");
                }
                let trap = self.trap_stmt(b.term_addr);
                let _ = write!(s, " default: {trap} }}");
                self.line(&s);
            }
            Term::Ret { pop } => {
                let fact = self.abs.rets.iter().find(|r| r.blk == bi as u32 && !r.is_tail);
                let pe = match fact {
                    Some(f) if f.esp == Some(0) => format!("{}u", pop),
                    _ => {
                        self.need_esp0 = true;
                        format!("(esp + {}u - esp0)", pop)
                    }
                };
                // x87 st0 return handled by FpRetStore ops inserted before the terminator
                let r = format!("guest_ret(eax, edx, {pe})");
                let s = self.wrap_ret(&r);
                self.line(&s);
            }
            Term::TailCall(t) => {
                let t = t.clone();
                self.emit_tail(&t, b.term_addr);
            }
            Term::Unreachable => self.line("__builtin_trap();"),
            Term::Trap { .. } => {
                let s = self.trap_stmt(b.term_addr);
                self.line(&s);
            }
        }
    }

    /// Emit the whole function; returns the C text.
    pub fn emit_function(&mut self) -> String {
        self.prepass();
        let f = self.f;
        // consumers first (collects flag variable usage)
        let mut keys: Vec<((u32, u32), &Op)> = Vec::new();
        for (bi, b) in f.blocks.iter().enumerate() {
            for (ii, ins) in b.insts.iter().enumerate() {
                if matches!(ins.op, Op::FlagGet { .. } | Op::FlagGetBit { .. } | Op::FlagsPack { .. }) {
                    keys.push(((bi as u32, ii as u32), &ins.op));
                }
            }
        }
        for (k, op) in keys {
            let rhs = self.consumer_rhs(k, op);
            self.cons_rhs.insert(k, rhs);
        }
        // body
        let mut body = String::new();
        std::mem::swap(&mut body, &mut self.out);
        for (bi, b) in f.blocks.iter().enumerate() {
            self.cur_blk = bi as u32;
            if self.block_target[bi] {
                let _ = writeln!(self.out, "L{bi}: ;");
            }
            for (ii, ins) in b.insts.iter().enumerate() {
                self.emit_op((bi as u32, ii as u32), ins);
            }
            self.emit_term(bi, b);
        }
        std::mem::swap(&mut body, &mut self.out);
        // header
        let mut h = String::new();
        let name = gname(f.entry);
        let ext = self.me.ext_in != 0 || self.me.ext_out != 0;
        let ret_ty = if ext { "LxExt" } else { "GuestRet" };
        let mut params = String::from("uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx");
        for g in EXT_ORDER {
            if self.me.ext_in & ext_bit(g) != 0 {
                params.push_str(&format!(", uint32_t {}", g.name()));
            }
        }
        let fname = if ext { format!("{name}_x") } else { name.clone() };
        let _ = writeln!(h, "{ret_ty} {fname}({params}) {{");
        if self.uses_mb {
            h.push_str("    uint8_t *const mb = guest_base;\n");
        }
        if self.uses_gt || body.contains("gt->") {
            h.push_str("    GuestThread *const gt = guest_thread;\n");
        }
        if self.uses_teb {
            h.push_str("    const uint32_t teb = gt->teb;\n");
        }
        if self.trace {
            h.push_str(&format!("    lx_trace_enter({:#x}u, esp, eax, ecx, edx);\n", f.entry));
        }
        if self.need_esp0 {
            h.push_str("    const uint32_t esp0 = esp;\n");
        }
        // callee-saved locals
        let mut locals: Vec<&str> = Vec::new();
        for g in EXT_ORDER {
            if self.gpr_used[g as usize] && self.me.ext_in & ext_bit(g) == 0 {
                locals.push(g.name());
            }
        }
        if !locals.is_empty() {
            let _ = writeln!(h, "    uint32_t {};", locals.iter().map(|n| format!("{n} = 0")).collect::<Vec<_>>().join(", "));
        }
        if self.uses_df_dyn {
            h.push_str("    uint32_t df = 0;\n");
        }
        if self.uses_pad {
            h.push_str("    GuestPad pad_ __attribute__((cleanup(guest_pad_leave))) = {0};\n");
        }
        // temporaries by type
        let mut by_ty: BTreeMap<u8, Vec<u32>> = BTreeMap::new();
        let mut used_tmp = vec![false; f.tmp_ty.len()];
        let scan = |p: Place, used: &mut Vec<bool>| {
            if let Place::Tmp(t) = p {
                used[t as usize] = true;
            }
        };
        for b in &f.blocks {
            for i in &b.insts {
                i.op.visit_uses(&mut |p| scan(p, &mut used_tmp));
                i.op.visit_defs(&mut |p| scan(p, &mut used_tmp));
            }
            b.term.visit_uses(&mut |p| scan(p, &mut used_tmp));
        }
        for (t, u) in used_tmp.iter().enumerate() {
            if *u {
                let k = match f.tmp_ty[t] {
                    Ty::I1 => 0,
                    Ty::I8 => 1,
                    Ty::I16 => 2,
                    Ty::I32 => 3,
                    Ty::I64 => 4,
                    Ty::F32 => 5,
                    Ty::F64 => 6,
                    Ty::V128 => 7,
                };
                by_ty.entry(k).or_default().push(t as u32);
            }
        }
        for (k, ts) in by_ty {
            let ty = [Ty::I1, Ty::I8, Ty::I16, Ty::I32, Ty::I64, Ty::F32, Ty::F64, Ty::V128][k as usize];
            let init = if ty == Ty::V128 { " = {0}" } else { " = 0" };
            let _ = writeln!(h, "    {} {};", crate::flagc::cty(ty), ts.iter().map(|t| format!("t{t}{init}")).collect::<Vec<_>>().join(", "));
        }
        // flag class variables
        for (cid, cl) in self.plan.classes.iter().enumerate() {
            if let Some((_, ty)) = cl.lazy {
                let used = self.lazy_used[cid];
                let t = crate::flagc::cty(ty);
                let mut v: Vec<String> = Vec::new();
                for (i, n) in ["a", "b", "r"].iter().enumerate() {
                    if used[i] {
                        v.push(format!("fc{cid}_{n} = 0"));
                    }
                }
                if !v.is_empty() {
                    let _ = writeln!(h, "    {t} {};", v.join(", "));
                }
                let mut v: Vec<String> = Vec::new();
                for (i, n) in ["ci", "cn"].iter().enumerate() {
                    if used[3 + i] {
                        v.push(format!("fc{cid}_{n} = 0"));
                    }
                }
                if !v.is_empty() {
                    let _ = writeln!(h, "    uint32_t {};", v.join(", "));
                }
            } else {
                let used = self.mat_used[cid];
                let v: Vec<String> = (0..6).filter(|k| used & (1 << k) != 0).map(|k| format!("fc{cid}_{} = 0", ["cf", "pf", "af", "zf", "sf", "of"][k])).collect();
                if !v.is_empty() {
                    let _ = writeln!(h, "    uint8_t {};", v.join(", "));
                }
            }
        }
        self.emit_fp_decls(&mut h);
        h.push_str(&body);
        h.push_str("}\n");
        if ext {
            // generic wrapper for dispatch
            let mut w = format!("GuestRet {name}(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {{\n");
            let mut args = String::from("esp, eax, ecx, edx");
            for (i, g) in EXT_ORDER.iter().enumerate() {
                if self.me.ext_in & ext_bit(*g) != 0 {
                    args.push_str(&format!(", guest_thread->ext_regs[{i}]"));
                }
            }
            if self.me.ext_in != 0 {
                w.push_str("    /* ext inputs: the registers the (indirect) caller handed over in GuestThread.ext_regs */\n");
            }
            let _ = writeln!(w, "    LxExt e_ = {fname}({args});");
            w.push_str("    return e_.r;\n}\n");
            h.push_str(&w);
        }
        h
    }
}
