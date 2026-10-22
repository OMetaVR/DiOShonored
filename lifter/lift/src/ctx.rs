//! Whole-program lifting context: discovery results for all modules, symbol tables, overrides.

use crate::config::OverrideCfg;
use crate::discover::{discover, Discovery, FlowEnv, JumpTable, Seeds};
use crate::program::{IatTarget, Program};
use ir::CallTarget;
use std::collections::{BTreeMap, HashMap, HashSet};

#[derive(Clone, Debug)]
pub struct Ovr {
    pub pop: Option<u16>,
    pub host: String,
    pub kind: OvrKind,
    pub name: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OvrKind {
    GuestFn,
    X87_1,
    X87_2,
    /// _ftol2_sse-style helper: eax = (int32) trunc(st0), st0 popped; no host call
    Ftol32,
    /// _ftol2-style helper: edx:eax = (int64) trunc(st0), st0 popped; no host call
    Ftol64,
}

pub struct LiftCtx {
    pub prog: Program,
    pub disc: Vec<Discovery>,
    pub entry_set: HashSet<u32>,
    pub jts: HashMap<u32, JumpTable>,
    pub overrides: HashMap<u32, Ovr>,
    pub noreturn_funcs: HashSet<u32>,
    pub noreturn_slots: HashSet<u32>,
    /// function entry -> module index
    pub entry_module: HashMap<u32, usize>,
    /// names for functions (exports) for diagnostics
    pub names: HashMap<u32, String>,
    pub syms: Vec<String>,
    /// tiny straight-line helpers (SEH/EH prolog/epilog style) inlined at their call sites: entry -> (instructions without the final ret, ret imm)
    pub inline_helpers: HashMap<u32, (Vec<iced_x86::Instruction>, u16)>,
    /// function entry -> EH continuations inside it (crate::ehcont): resumed through a landing pad, never lifted as functions
    pub resume: HashMap<u32, Vec<crate::ehcont::Cont>>,
}

impl LiftCtx {
    pub fn build(prog: Program, log: &mut dyn FnMut(String)) -> LiftCtx {
        let mut disc = Vec::new();
        let mut resume_conts: HashMap<u32, Vec<crate::ehcont::Cont>> = HashMap::new();
        let noreturn_funcs = prog.noreturn_funcs.clone();
        let noreturn_slots = prog.noreturn_slots.clone();
        for (mi, img) in prog.images.iter().enumerate() {
            let mc = &prog.cfg.modules[mi];
            let env = FlowEnv { noreturn_slots: &noreturn_slots, noreturn_funcs: &noreturn_funcs, data_ranges: &mc.data_ranges };
            let conts = crate::ehcont::scan(img);
            let mut not_entries: std::collections::HashSet<u32> = mc.not_entries.iter().copied().collect();
            not_entries.extend(conts.iter().map(|c| c.addr));
            let seeds = Seeds { config_entries: mc.entries.clone(), not_entries };
            let d = discover(img, &env, &seeds, &mut |s| log(format!("[{}] {s}", img.name)));
            // owner: the function whose prolog holds the stub / scope-table push
            for c in conts {
                match d.entries.range(..=c.site).next_back() {
                    Some(&f) => resume_conts.entry(f).or_insert_with(Vec::new).push(c),
                    None => log(format!("[{}] EH continuation {:08x}: no owner function for site {:08x}", img.name, c.addr, c.site)),
                }
            }
            disc.push(d);
        }
        log(format!("EH continuations: {} in {} functions", resume_conts.values().map(|v| v.len()).sum::<usize>(), resume_conts.len()));
        let mut entry_set = HashSet::new();
        let mut entry_module = HashMap::new();
        let mut jts = HashMap::new();
        for (mi, d) in disc.iter().enumerate() {
            for &e in &d.entries {
                entry_set.insert(e);
                entry_module.insert(e, mi);
            }
            for (k, v) in &d.jts {
                jts.insert(*k, v.clone());
            }
        }
        let mut names = HashMap::new();
        for img in &prog.images {
            for e in &img.exports {
                if e.va != 0 {
                    if let Some(n) = &e.name {
                        names.entry(e.va).or_insert_with(|| format!("{}!{}", img.name, n));
                    }
                }
            }
        }
        // overrides
        let mut overrides = HashMap::new();
        for o in &prog.cfg.overrides {
            let addr = resolve_ovr(&prog, o);
            if let Some(a) = addr {
                let kind = match o.kind.as_str() {
                    "x87_1" => OvrKind::X87_1,
                    "x87_2" => OvrKind::X87_2,
                    "x87_ftol32" => OvrKind::Ftol32,
                    "x87_ftol64" => OvrKind::Ftol64,
                    _ => OvrKind::GuestFn,
                };
                overrides.insert(a, Ovr { pop: o.pop, host: o.host.clone(), kind, name: o.export.clone().unwrap_or_default() });
            } else {
                log(format!("override {} not found in {}", o.export.clone().unwrap_or_default(), o.module));
            }
        }
        for h in &prog.cfg.hooks {
            overrides.insert(h.addr, Ovr { pop: Some(h.pop), host: h.host.clone(), kind: OvrKind::GuestFn, name: format!("hook_{:08x}", h.addr) });
        }
        let mut syms: Vec<String> = overrides.values().map(|o| o.host.clone()).collect();
        syms.sort();
        syms.dedup();
        LiftCtx { prog, disc, entry_set, jts, overrides, noreturn_funcs, noreturn_slots, entry_module, names, syms, inline_helpers: HashMap::new(), resume: resume_conts }
    }

    /// stable index of a host symbol name (sorted at construction)
    pub fn sym_index(&self, host: &str) -> u32 {
        self.syms.binary_search_by(|x| x.as_str().cmp(host)).map(|i| i as u32).unwrap_or(u32::MAX)
    }
    pub fn sym_name(&self, i: u32) -> String {
        self.syms.get(i as usize).cloned().unwrap_or_default()
    }

    pub fn module_of(&self, va: u32) -> Option<usize> {
        self.prog.image_of(va)
    }

    pub fn iat_target(&self, slot: u32) -> Option<CallTarget> {
        match self.prog.iat.get(&slot)? {
            IatTarget::Guest(a) => Some(CallTarget::Direct(*a)),
            IatTarget::Import(i) => Some(CallTarget::Hle(*i)),
        }
    }

    /// `a` is an import stub (a lone `jmp dword ptr [slot]`, as the linker emits for `call import`): the slot's target. Calls to the
    /// stub are lifted as calls through the slot, so x87 intrinsics (_CIpow, ...) and direct HLE calls apply to them as well.
    pub fn import_stub_target(&self, a: u32) -> Option<CallTarget> {
        let img = &self.prog.images[self.prog.image_of(a)?];
        let r = (a - img.base) as usize;
        let b = img.mem.get(r..r + 6)?;
        if b[0] != 0xFF || b[1] != 0x25 {
            return None;
        }
        self.iat_target(u32::from_le_bytes([b[2], b[3], b[4], b[5]]))
    }

    pub fn hle_noreturn(&self, idx: u32) -> bool {
        let im = &self.prog.imports[idx as usize];
        match &im.name {
            Some(n) => self.prog.cfg.noreturn_imports.iter().any(|x| *x == format!("{}!{}", im.dll, n)),
            None => false,
        }
    }

    pub fn sorted_entries(&self, mi: usize) -> Vec<u32> {
        self.disc[mi].entries.iter().copied().collect()
    }

    pub fn all_funcs(&self) -> BTreeMap<u32, usize> {
        let mut m = BTreeMap::new();
        for (mi, d) in self.disc.iter().enumerate() {
            for &e in &d.entries {
                m.insert(e, mi);
            }
        }
        m
    }
}

fn resolve_ovr(prog: &Program, o: &OverrideCfg) -> Option<u32> {
    if let Some(a) = o.addr {
        return Some(a);
    }
    let e = o.export.as_ref()?;
    prog.export_by_name.get(&(o.module.to_lowercase(), e.clone())).copied()
}
