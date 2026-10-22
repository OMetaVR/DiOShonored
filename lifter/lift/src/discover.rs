//! Entry discovery for one module: seeds (entry, exports, config, .reloc pointers), recursive descent, jump tables from
//! .reloc runs, gap prologue scan. Port of tools/analysis/discover.py to Rust.

use crate::image::Image;
use iced_x86::{Decoder, DecoderOptions, FlowControl, Instruction, Mnemonic, OpKind, Register};
use std::collections::{BTreeSet, HashMap, HashSet};

pub const SRC_EXPORT: u32 = 1;
pub const SRC_ENTRYPOINT: u32 = 2;
pub const SRC_CALL: u32 = 4;
pub const SRC_RELOC_DATA: u32 = 8;
pub const SRC_RELOC_CODE: u32 = 16;
pub const SRC_GAP: u32 = 32;
pub const SRC_CONFIG: u32 = 64;
pub const SRC_JMP: u32 = 128;

#[derive(Clone, Debug)]
pub struct JumpTable {
    pub site: u32,
    pub table: u32,
    pub targets: Vec<u32>,
    pub bounded: bool, // length derived from a preceding cmp, not only from the reloc run
    pub reloc_run: usize,
}

/// Facts about the surroundings of the module needed for flow decisions.
pub struct FlowEnv<'a> {
    /// IAT slot -> "dll!name" for noreturn imports (slot VA set)
    pub noreturn_slots: &'a HashSet<u32>,
    /// absolute addresses of functions known not to return
    pub noreturn_funcs: &'a HashSet<u32>,
    pub data_ranges: &'a [[u32; 2]],
}

pub fn is_noise(m: Mnemonic) -> bool {
    use Mnemonic::*;
    matches!(
        m,
        In | Out | Insb | Insw | Insd | Outsb | Outsw | Outsd | Hlt | Bound | Enter | Arpl | Daa | Das | Aaa | Aas | Aam | Aad | Lds | Les | Into | Salc | Iret | Iretd | Iretq | Retf | Pusha | Popa | Pushad | Popad | Sti | Cli | Int | Int1 | Sldt | Fbld | Fbstp | Loop | Loope | Loopne
    )
}

pub struct Explorer<'a> {
    pub img: &'a Image,
    dec: Decoder<'a>,
    stamp: Vec<u32>,
    epoch: u32,
    code_lo: u32,
    code_hi: u32,
    reloc_set: HashSet<u32>,
}

#[derive(Default, Debug)]
pub struct ExploreOut {
    pub instrs: Vec<(u32, u8)>,
    pub calls: Vec<u32>,
    pub jmps: Vec<(u32, u32, bool)>, // site, target, conditional
    pub jts: Vec<JumpTable>,
    pub unresolved_ind: Vec<u32>,
    pub invalid: Vec<u32>,
    pub noise: Vec<u32>,
}

impl<'a> Explorer<'a> {
    pub fn new(img: &'a Image) -> Explorer<'a> {
        let dec = Decoder::with_ip(32, &img.mem, img.base as u64, DecoderOptions::NONE);
        let rs = img.code_ranges();
        let lo = rs.iter().map(|r| r.0).min().unwrap_or(0);
        let hi = rs.iter().map(|r| r.1).max().unwrap_or(0);
        Explorer { img, dec, stamp: vec![0; img.mem.len()], epoch: 0, code_lo: lo, code_hi: hi, reloc_set: img.relocs.iter().copied().collect() }
    }
    #[inline]
    pub fn in_code(&self, va: u32) -> bool {
        va >= self.code_lo && va < self.code_hi && self.img.in_code(va)
    }
    #[inline]
    pub fn decode_at(&mut self, va: u32) -> Instruction {
        let _ = self.dec.set_position((va - self.img.base) as usize);
        self.dec.set_ip(va as u64);
        self.dec.decode()
    }

    pub fn read_jump_table(&self, tbl: u32, max: usize) -> Vec<u32> {
        let mut out = Vec::new();
        let mut va = tbl;
        while out.len() < max && self.reloc_set.contains(&va) {
            match self.img.read32(va) {
                Some(t) if self.in_code(t) => out.push(t),
                _ => break,
            }
            va += 4;
        }
        out
    }

    /// Recursive descent from `entry`. If `stop` returns true for an address (other than the entry) the walk does not
    /// continue into it (used for the final partition); in discovery rounds `stop` is None.
    pub fn explore(&mut self, entry: u32, env: &FlowEnv, stop: Option<&dyn Fn(u32) -> bool>) -> ExploreOut {
        self.epoch += 1;
        let ep = self.epoch;
        let mut out = ExploreOut::default();
        let mut work = vec![entry];
        while let Some(start) = work.pop() {
            let mut va = start;
            let mut ring: [Option<Instruction>; 10] = Default::default();
            let mut rn = 0usize;
            loop {
                if !self.in_code(va) {
                    break;
                }
                if env.data_ranges.iter().any(|r| va >= r[0] && va < r[1]) {
                    break;
                }
                let rva = (va - self.img.base) as usize;
                if self.stamp[rva] == ep {
                    break;
                }
                if va != entry && va != start {
                    if let Some(s) = stop {
                        if s(va) {
                            break;
                        }
                    }
                } else if va == start && va != entry {
                    if let Some(s) = stop {
                        if s(va) {
                            break;
                        }
                    }
                }
                let i = self.decode_at(va);
                if i.is_invalid() {
                    out.invalid.push(va);
                    break;
                }
                if is_noise(i.mnemonic()) {
                    out.noise.push(va);
                    break;
                }
                self.stamp[rva] = ep;
                let len = i.len();
                out.instrs.push((va, len as u8));
                let nxt = va.wrapping_add(len as u32);
                ring[rn % 10] = Some(i);
                rn += 1;
                match i.flow_control() {
                    FlowControl::Next => {
                        va = nxt;
                        continue;
                    }
                    FlowControl::Call => {
                        let t = i.near_branch_target() as u32;
                        if t != nxt && self.in_code(t) {
                            out.calls.push(t);
                            if env.noreturn_funcs.contains(&t) {
                                break;
                            }
                        }
                        va = nxt;
                        continue;
                    }
                    FlowControl::IndirectCall => {
                        if i.op0_kind() == OpKind::Memory && i.memory_base() == Register::None && i.memory_index() == Register::None {
                            if env.noreturn_slots.contains(&i.memory_displacement32()) {
                                break;
                            }
                        }
                        va = nxt;
                        continue;
                    }
                    FlowControl::ConditionalBranch => {
                        let t = i.near_branch_target() as u32;
                        out.jmps.push((va, t, true));
                        if stop.map_or(true, |s| t == entry || !s(t)) {
                            work.push(t);
                        }
                        va = nxt;
                        continue;
                    }
                    FlowControl::UnconditionalBranch => {
                        let t = i.near_branch_target() as u32;
                        out.jmps.push((va, t, false));
                        if stop.map_or(true, |s| t == entry || !s(t)) {
                            work.push(t);
                        }
                        break;
                    }
                    FlowControl::IndirectBranch => {
                        if i.op0_kind() == OpKind::Memory
                            && i.memory_index() != Register::None
                            && i.memory_index_scale() == 4
                            && i.memory_base() == Register::None
                        {
                            let tbl = i.memory_displacement32();
                            let run = self.read_jump_table(tbl, 8192);
                            if !run.is_empty() {
                                let (n, bounded) = self.jt_bound(&ring, rn, &i, run.len());
                                let tg: Vec<u32> = run[..n].to_vec();
                                for &t in &tg {
                                    work.push(t);
                                }
                                out.jts.push(JumpTable { site: va, table: tbl, targets: tg, bounded, reloc_run: run.len() });
                            } else {
                                out.unresolved_ind.push(va);
                            }
                        } else if i.op0_kind() == OpKind::Memory
                            && i.memory_base() == Register::None
                            && i.memory_index() == Register::None
                        {
                            // jmp [abs]: import thunk / tail call through pointer
                        } else {
                            out.unresolved_ind.push(va);
                        }
                        break;
                    }
                    FlowControl::Return | FlowControl::Interrupt | FlowControl::Exception => {
                        break;
                    }
                    _ => {
                        va = nxt;
                        continue;
                    }
                }
            }
        }
        out
    }

    /// Decide the number of table entries from a preceding `cmp idx, imm; ja/jae default` if present.
    fn jt_bound(&self, ring: &[Option<Instruction>; 10], rn: usize, jmp: &Instruction, run: usize) -> (usize, bool) {
        let idx = jmp.memory_index();
        let full = |r: Register| r.full_register32();
        let mut want = full(idx);
        let mut max_from_bytes: Option<usize> = None;
        let mut k = 1usize;
        // walk back through the ring (most recent first); skip the jmp itself
        let mut cmp_val: Option<(usize, bool)> = None; // (limit, inclusive)
        let n_avail = rn.min(10);
        let get = |k: usize| -> Option<Instruction> { ring[(rn - 1 - k) % 10] };
        while k < n_avail {
            let ins = match get(k) {
                Some(x) => x,
                None => break,
            };
            // movzx idx, byte [reg + tbl2]: the real bound applies to `reg`; the table of bytes gives the max value
            if ins.mnemonic() == Mnemonic::Movzx
                && ins.op0_kind() == OpKind::Register
                && full(ins.op0_register()) == want
                && ins.op1_kind() == OpKind::Memory
                && ins.memory_size() == iced_x86::MemorySize::UInt8
                && ins.memory_index() == Register::None
                && ins.memory_base() != Register::None
            {
                max_from_bytes = Some(ins.memory_displacement32() as usize);
                want = full(ins.memory_base());
                // keep looking for the cmp on `want`
            } else if ins.mnemonic() == Mnemonic::Cmp
                && ins.op0_kind() == OpKind::Register
                && full(ins.op0_register()) == want
                && matches!(ins.op1_kind(), OpKind::Immediate8 | OpKind::Immediate8to32 | OpKind::Immediate32)
            {
                // find the conditional branch that follows it (toward the jmp)
                if k >= 1 {
                    if let Some(br) = get(k - 1) {
                        let imm = ins.immediate(1) as u32 as usize;
                        match br.mnemonic() {
                            Mnemonic::Ja => cmp_val = Some((imm + 1, true)),
                            Mnemonic::Jae => cmp_val = Some((imm, true)),
                            _ => {}
                        }
                    }
                }
                break;
            } else if ins.mnemonic() == Mnemonic::Sub
                && ins.op0_kind() == OpKind::Register
                && full(ins.op0_register()) == want
            {
                // index adjusted before cmp: keep going (cmp is on the same register)
            } else if ins.mnemonic() == Mnemonic::Lea && full(ins.op0_register()) == want && ins.memory_index() == Register::None {
                want = full(ins.memory_base());
            } else if ins.op0_kind() == OpKind::Register && full(ins.op0_register()) == want && !matches!(ins.mnemonic(), Mnemonic::Cmp | Mnemonic::Test) {
                // some other redefinition: stop
                break;
            }
            k += 1;
        }
        match (cmp_val, max_from_bytes) {
            (Some((n, _)), Some(tb)) => {
                // byte table: read n bytes at tb; entries = max+1
                let mut m = 0usize;
                let mut ok = true;
                for j in 0..n.min(512) {
                    match self.img.read8(tb as u32 + j as u32) {
                        Some(b) => m = m.max(b as usize),
                        None => {
                            ok = false;
                            break;
                        }
                    }
                }
                if ok && m + 1 <= run {
                    (m + 1, true)
                } else {
                    (run, false)
                }
            }
            (Some((n, _)), None) => {
                if n >= 1 && n <= run {
                    (n, true)
                } else {
                    (run, false)
                }
            }
            _ => (run, false),
        }
    }
}

pub struct Discovery {
    pub entries: BTreeSet<u32>,
    pub src: HashMap<u32, u32>,
    pub jts: HashMap<u32, JumpTable>, // by site
    pub covered: Vec<u8>,             // per image byte: 1 = instruction byte, 2 = jump-table data
    pub jt_slots: HashSet<u32>,
}

const PROLOGUES: &[&[u8]] = &[
    b"\x55\x8b\xec", b"\x8b\xff\x55\x8b\xec", b"\x83\xec", b"\x6a\xff\x68", b"\x6a\x00\x68", b"\x56\x57", b"\x53\x56", b"\x51\x53",
    b"\x51\x56", b"\x81\xec", b"\x8b\x44\x24", b"\x8b\x4c\x24", b"\x53", b"\x56", b"\x57", b"\x51", b"\x55", b"\x33\xc0", b"\x32\xc0",
    b"\xb8", b"\x6a", b"\xf6\x44", b"\x8b\x01", b"\x8b\x41", b"\xd9", b"\xf3", b"\x0f",
];

pub struct Seeds {
    pub config_entries: Vec<u32>,
    pub not_entries: HashSet<u32>,
}

pub fn discover(img: &Image, env: &FlowEnv, seeds: &Seeds, log: &mut dyn FnMut(String)) -> Discovery {
    let mut ex = Explorer::new(img);
    let mut src: HashMap<u32, u32> = HashMap::new();
    let mut add = |src: &mut HashMap<u32, u32>, e: u32, s: u32| {
        *src.entry(e).or_insert(0) |= s;
    };
    // reloc-derived pointers into code
    let mut reloc_ptrs: Vec<(u32, u32)> = Vec::new(); // (site, target)
    for &r in &img.relocs {
        if let Some(v) = img.read32(r) {
            if ex.in_code(v) && !seeds.not_entries.contains(&v) {
                reloc_ptrs.push((r, v));
            }
        }
    }
    log(format!("reloc->code pointers: {}", reloc_ptrs.len()));
    let mut base_entries: HashMap<u32, u32> = HashMap::new();
    if img.entry != 0 && ex.in_code(img.entry) {
        add(&mut base_entries, img.entry, SRC_ENTRYPOINT);
    }
    for e in &img.exports {
        if e.va != 0 && ex.in_code(e.va) {
            add(&mut base_entries, e.va, SRC_EXPORT);
        }
    }
    for &c in &img.tls_callbacks {
        add(&mut base_entries, c, SRC_ENTRYPOINT);
    }
    for &c in &seeds.config_entries {
        add(&mut base_entries, c, SRC_CONFIG);
    }
    let mut jt_slots: HashSet<u32> = HashSet::new();
    let mut jt_targets: HashSet<u32> = HashSet::new();
    let reloc_src = |site: u32| -> u32 {
        match img.section_of(site) {
            Some(s) if img.is_exec_section(s) => SRC_RELOC_CODE,
            _ => SRC_RELOC_DATA,
        }
    };
    let mut entries: HashMap<u32, u32> = base_entries.clone();
    for &(site, t) in &reloc_ptrs {
        add(&mut entries, t, reloc_src(site));
    }
    log(format!("initial entries: {}", entries.len()));
    let mut all_jts: HashMap<u32, JumpTable> = HashMap::new();
    for it in 0..5 {
        // round 1: explore everything, following all jumps; call targets become entries
        let mut work: Vec<u32> = entries.keys().copied().collect();
        work.sort_unstable_by(|a, b| b.cmp(a));
        let mut done: HashSet<u32> = HashSet::new();
        let mut jts_round: HashMap<u32, JumpTable> = HashMap::new();
        let mut call_found: HashSet<u32> = HashSet::new();
        while let Some(e) = work.pop() {
            if !done.insert(e) {
                continue;
            }
            let o = ex.explore(e, env, None);
            for c in o.calls {
                call_found.insert(c);
                if !entries.contains_key(&c) {
                    entries.insert(c, SRC_CALL);
                    work.push(c);
                } else {
                    *entries.get_mut(&c).unwrap() |= SRC_CALL;
                }
            }
            for j in o.jts {
                jts_round.entry(j.site).or_insert(j);
            }
        }
        let before = jt_slots.len();
        for jt in jts_round.values() {
            for k in 0..jt.targets.len() {
                jt_slots.insert(jt.table + 4 * k as u32);
            }
            // slots beyond the bounded length are still table data: mark the full reloc run as table slots
            for k in jt.targets.len()..jt.reloc_run {
                jt_slots.insert(jt.table + 4 * k as u32);
            }
            jt_targets.extend(jt.targets.iter().copied());
        }
        all_jts.extend(jts_round);
        // seeds that come only through jump-table slots are not entries
        let mut outside: HashMap<u32, u32> = HashMap::new();
        for &(site, t) in &reloc_ptrs {
            if !jt_slots.contains(&site) {
                add(&mut outside, t, reloc_src(site));
            }
        }
        let mut next: HashMap<u32, u32> = HashMap::new();
        for (&e, &s) in &entries {
            let keep = !jt_targets.contains(&e) || outside.contains_key(&e) || base_entries.contains_key(&e) || call_found.contains(&e);
            if keep {
                // drop reloc flags if all relocs were jump-table slots
                let mut s2 = s & !(SRC_RELOC_CODE | SRC_RELOC_DATA);
                if let Some(o) = outside.get(&e) {
                    s2 |= o;
                }
                next.insert(e, s2);
            }
        }
        for (&e, &s) in &outside {
            *next.entry(e).or_insert(0) |= s;
        }
        for (&e, &s) in &base_entries {
            *next.entry(e).or_insert(0) |= s;
        }
        entries = next;
        log(format!("iter {it}: entries={} jt_slots={} jt_targets={}", entries.len(), jt_slots.len(), jt_targets.len()));
        if jt_slots.len() == before && it > 0 {
            break;
        }
    }
    // full round 1 once more to get covered bytes
    let mut covered = vec![0u8; img.mem.len()];
    let mut work: Vec<u32> = entries.keys().copied().collect();
    work.sort_unstable_by(|a, b| b.cmp(a));
    let mut done: HashSet<u32> = HashSet::new();
    let mut mark = |ex: &mut Explorer, covered: &mut Vec<u8>, o: &ExploreOut| {
        let _ = ex;
        for &(va, l) in &o.instrs {
            let r = (va - img.base) as usize;
            for b in &mut covered[r..r + l as usize] {
                *b = 1;
            }
        }
    };
    while let Some(e) = work.pop() {
        if !done.insert(e) {
            continue;
        }
        let o = ex.explore(e, env, None);
        mark(&mut ex, &mut covered, &o);
        for c in &o.calls {
            if !entries.contains_key(c) {
                entries.insert(*c, SRC_CALL);
                work.push(*c);
            }
        }
        for j in o.jts {
            all_jts.entry(j.site).or_insert(j);
        }
    }
    for jt in all_jts.values() {
        if jt.table >= img.base && img.in_code(jt.table) {
            for k in 0..jt.reloc_run.max(jt.targets.len()) * 4 {
                let r = (jt.table - img.base) as usize + k;
                if r < covered.len() && covered[r] == 0 {
                    covered[r] = 2;
                }
            }
        }
    }
    // gap prologue scan
    for gi in 0..6 {
        let mut gaps: Vec<(u32, u32)> = Vec::new();
        for (lo, hi) in img.code_ranges() {
            let mut o = lo;
            while o < hi {
                if covered[(o - img.base) as usize] != 0 {
                    o += 1;
                    continue;
                }
                let s = o;
                while o < hi && covered[(o - img.base) as usize] == 0 {
                    o += 1;
                }
                gaps.push((s, o));
            }
        }
        let mut added = 0;
        for (s, e) in &gaps {
            let mut p = *s;
            while p < *e {
                let b = img.mem[(p - img.base) as usize];
                if b == 0xcc || b == 0x90 || b == 0x00 {
                    p += 1;
                    continue;
                }
                let q = ex.decode_at(p);
                if q.is_invalid() {
                    break;
                }
                let nop_like = q.mnemonic() == Mnemonic::Nop
                    || (q.mnemonic() == Mnemonic::Lea
                        && q.op0_register() == q.memory_base()
                        && q.memory_index() == Register::None
                        && q.memory_displacement32() == 0)
                    || (q.mnemonic() == Mnemonic::Mov
                        && q.op0_kind() == OpKind::Register
                        && q.op1_kind() == OpKind::Register
                        && q.op0_register() == q.op1_register()
                        && img.bytes(p, 5) != Some(b"\x8b\xff\x55\x8b\xec"));
                if nop_like {
                    p += q.len() as u32;
                    continue;
                }
                break;
            }
            if p >= *e {
                continue;
            }
            if entries.contains_key(&p) || seeds.not_entries.contains(&p) {
                continue;
            }
            let bytes = img.bytes(p, 8).unwrap_or(&[]);
            if !PROLOGUES.iter().any(|pr| bytes.starts_with(pr)) {
                continue;
            }
            let o = ex.explore(p, env, None);
            if o.instrs.len() >= 3 {
                let ok = o.instrs.iter().all(|&(a, _)| covered[(a - img.base) as usize] == 0);
                if ok {
                    entries.insert(p, SRC_GAP);
                    added += 1;
                    mark(&mut ex, &mut covered, &o);
                    for c in &o.calls {
                        if !entries.contains_key(c) && covered[(*c - img.base) as usize] == 0 {
                            entries.insert(*c, SRC_CALL);
                        }
                    }
                    for j in o.jts {
                        all_jts.entry(j.site).or_insert(j);
                    }
                }
            }
        }
        // new entries from calls inside gap functions: explore those
        let mut work: Vec<u32> = entries.keys().copied().filter(|e| covered[(*e - img.base) as usize] == 0).collect();
        while let Some(e) = work.pop() {
            let o = ex.explore(e, env, None);
            mark(&mut ex, &mut covered, &o);
            for c in &o.calls {
                if !entries.contains_key(c) {
                    entries.insert(*c, SRC_CALL);
                    work.push(*c);
                }
            }
            for j in o.jts {
                all_jts.entry(j.site).or_insert(j);
            }
        }
        log(format!("gap pass {gi}: gaps={} added={added}", gaps.len()));
        if added == 0 {
            break;
        }
    }
    let mut ents = BTreeSet::new();
    for (&e, &s) in &entries {
        if ex.in_code(e) && !seeds.not_entries.contains(&e) {
            ents.insert(e);
            src.insert(e, s);
        }
    }
    Discovery { entries: ents, src, jts: all_jts, covered, jt_slots }
}
