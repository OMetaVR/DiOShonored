//! Lazy EFLAGS: reaching-definition analysis over flag producers (`Op::FlagDef`) and consumers, and partitioning of
//! producers into "classes" that share C variables. A class with a single producer kind/type keeps the operands
//! (a, b, r, ...) and consumers are fused into direct comparisons; other classes materialise the consumed bits.

use ir::*;
use std::collections::HashMap;

#[derive(Clone, Debug)]
pub struct Prod {
    pub blk: u32,
    pub idx: u32,
    pub kind: FlagKind,
    pub ty: Ty,
    pub mask: u8,
    pub partial: bool,
}

#[derive(Clone, Debug, Default)]
pub struct FClass {
    pub members: Vec<u32>, // pids (1-based)
    /// Some((kind, ty)) when all members share them: operands are kept (lazy); otherwise bits are materialised
    pub lazy: Option<(FlagKind, Ty)>,
    pub has_entry: bool,
    /// bits (FL_*) read by consumers from this class
    pub bits_used: u8,
}

#[derive(Clone, Debug)]
pub struct ConsInfo {
    /// per flag bit index (CF,PF,AF,ZF,SF,OF): class id or -1 (constant 0)
    pub bit_class: [i32; 6],
}

#[derive(Default, Debug)]
pub struct FlagPlan {
    pub prods: Vec<Prod>,
    pub class_of: Vec<i32>, // indexed by pid-1
    pub classes: Vec<FClass>,
    pub cons: HashMap<(u32, u32), ConsInfo>,
    pub prod_at: HashMap<(u32, u32), u32>, // (blk, idx) -> pid
    pub uses_df: bool,
}

type Bits = Vec<u64>;

fn set1(b: &mut Bits, i: usize) {
    b[i >> 6] |= 1u64 << (i & 63);
}
fn clear(b: &mut Bits) {
    for w in b.iter_mut() {
        *w = 0;
    }
}
fn or_into(dst: &mut Bits, src: &Bits) -> bool {
    let mut ch = false;
    for (d, s) in dst.iter_mut().zip(src.iter()) {
        let n = *d | *s;
        if n != *d {
            *d = n;
            ch = true;
        }
    }
    ch
}

type St = [Bits; 6];

struct Uf {
    p: Vec<u32>,
}
impl Uf {
    fn new(n: usize) -> Uf {
        Uf { p: (0..n as u32).collect() }
    }
    fn find(&mut self, x: u32) -> u32 {
        let mut r = x;
        while self.p[r as usize] != r {
            r = self.p[r as usize];
        }
        let mut c = x;
        while self.p[c as usize] != r {
            let n = self.p[c as usize];
            self.p[c as usize] = r;
            c = n;
        }
        r
    }
    fn union(&mut self, a: u32, b: u32) {
        let (a, b) = (self.find(a), self.find(b));
        if a != b {
            self.p[b as usize] = a;
        }
    }
}

fn consumer_bits(op: &Op) -> Option<u8> {
    match op {
        Op::FlagGet { cond, .. } => Some(cond.flags()),
        Op::FlagGetBit { flag, .. } => Some(*flag),
        Op::FlagsPack { .. } => Some(FL_ALL),
        _ => None,
    }
}

pub fn plan(func: &Func) -> FlagPlan {
    let mut fp = FlagPlan::default();
    // enumerate producers and consumers
    let mut any_cons = false;
    for (bi, b) in func.blocks.iter().enumerate() {
        for (ii, ins) in b.insts.iter().enumerate() {
            match &ins.op {
                Op::FlagDef(d) => {
                    fp.prods.push(Prod { blk: bi as u32, idx: ii as u32, kind: d.kind, ty: d.ty, mask: d.mask, partial: d.cond_cnt });
                    fp.prod_at.insert((bi as u32, ii as u32), fp.prods.len() as u32);
                }
                Op::SetDf(_) | Op::GetDf { .. } => fp.uses_df = true,
                _ => {
                    if consumer_bits(&ins.op).is_some() {
                        any_cons = true;
                    }
                }
            }
        }
    }
    fp.class_of = vec![-1; fp.prods.len()];
    if fp.prods.is_empty() || !any_cons {
        // consumers (if any) read constant 0
        for (bi, b) in func.blocks.iter().enumerate() {
            for (ii, ins) in b.insts.iter().enumerate() {
                if consumer_bits(&ins.op).is_some() {
                    fp.cons.insert((bi as u32, ii as u32), ConsInfo { bit_class: [-1; 6] });
                }
            }
        }
        // Br terminators reference temps computed by FlagGet; nothing else to do
        return fp;
    }
    // terminators that read flags do not exist (conditions are computed by FlagGet ops)
    let np = fp.prods.len() + 1;
    let words = (np + 63) / 64;
    let nb = func.blocks.len();
    let mut ins_state: Vec<Option<St>> = vec![None; nb];
    let mut init: St = std::array::from_fn(|_| vec![0u64; words]);
    for b in 0..6 {
        set1(&mut init[b], 0);
    }
    ins_state[0] = Some(init);
    let mut work: Vec<u32> = vec![0];
    let mut inwork = vec![false; nb];
    inwork[0] = true;
    let transfer = |st: &mut St, bi: usize, fp: &FlagPlan, func: &Func| {
        for (ii, ins) in func.blocks[bi].insts.iter().enumerate() {
            match &ins.op {
                Op::FlagDef(d) => {
                    let pid = fp.prod_at[&(bi as u32, ii as u32)] as usize;
                    for k in 0..6 {
                        if d.mask & (1 << k) != 0 {
                            if !d.cond_cnt {
                                clear(&mut st[k]);
                            }
                            set1(&mut st[k], pid);
                        }
                    }
                }
                Op::Call { .. } => {
                    for k in 0..6 {
                        clear(&mut st[k]);
                        set1(&mut st[k], 0);
                    }
                }
                _ => {}
            }
        }
    };
    while let Some(b) = work.pop() {
        let b = b as usize;
        inwork[b] = false;
        let mut st = ins_state[b].clone().unwrap();
        transfer(&mut st, b, &fp, func);
        for s in func.blocks[b].term.succs() {
            let s = s as usize;
            let changed = match &mut ins_state[s] {
                None => {
                    ins_state[s] = Some(st.clone());
                    true
                }
                Some(cur) => {
                    let mut ch = false;
                    for k in 0..6 {
                        ch |= or_into(&mut cur[k], &st[k]);
                    }
                    ch
                }
            };
            if changed && !inwork[s] {
                inwork[s] = true;
                work.push(s as u32);
            }
        }
    }
    // collect consumer def-sets
    let mut uf = Uf::new(np);
    struct Cons {
        key: (u32, u32),
        sets: [Vec<u32>; 6],
        need: u8,
    }
    let mut conss: Vec<Cons> = Vec::new();
    for bi in 0..nb {
        let mut st = match &ins_state[bi] {
            Some(s) => s.clone(),
            None => continue, // unreachable block
        };
        for (ii, ins) in func.blocks[bi].insts.iter().enumerate() {
            if let Some(need) = consumer_bits(&ins.op) {
                let mut sets: [Vec<u32>; 6] = Default::default();
                for k in 0..6 {
                    if need & (1 << k) != 0 {
                        for (wi, w) in st[k].iter().enumerate() {
                            let mut w = *w;
                            while w != 0 {
                                let t = w.trailing_zeros() as usize;
                                sets[k].push((wi * 64 + t) as u32);
                                w &= w - 1;
                            }
                        }
                    }
                }
                conss.push(Cons { key: (bi as u32, ii as u32), sets, need });
            }
            match &ins.op {
                Op::FlagDef(d) => {
                    let pid = fp.prod_at[&(bi as u32, ii as u32)] as usize;
                    for k in 0..6 {
                        if d.mask & (1 << k) != 0 {
                            if !d.cond_cnt {
                                clear(&mut st[k]);
                            }
                            set1(&mut st[k], pid);
                        }
                    }
                }
                Op::Call { .. } => {
                    for k in 0..6 {
                        clear(&mut st[k]);
                        set1(&mut st[k], 0);
                    }
                }
                _ => {}
            }
        }
    }
    // union producers that reach the same consumer bit
    for c in &conss {
        for k in 0..6 {
            let real: Vec<u32> = c.sets[k].iter().copied().filter(|&p| p != 0).collect();
            for w in real.windows(2) {
                uf.union(w[0], w[1]);
            }
        }
    }
    // class ids
    let mut class_id: HashMap<u32, i32> = HashMap::new();
    fp.class_of = vec![-1; np - 1];
    let mut used_producers = vec![false; np];
    for c in &conss {
        for k in 0..6 {
            for &p in &c.sets[k] {
                if p != 0 {
                    used_producers[p as usize] = true;
                }
            }
        }
    }
    for pid in 1..np {
        if !used_producers[pid] {
            continue;
        }
        let r = uf.find(pid as u32);
        let n = class_id.len() as i32;
        let cid = *class_id.entry(r).or_insert(n);
        if cid as usize >= fp.classes.len() {
            fp.classes.push(FClass::default());
        }
        fp.classes[cid as usize].members.push(pid as u32);
        fp.class_of[pid - 1] = cid;
    }
    for cl in fp.classes.iter_mut() {
        let first = &fp.prods[cl.members[0] as usize - 1];
        let same = cl.members.iter().all(|&m| {
            let p = &fp.prods[m as usize - 1];
            p.kind == first.kind && p.ty == first.ty
        });
        cl.lazy = if same { Some((first.kind, first.ty)) } else { None };
    }
    for c in &conss {
        let mut info = ConsInfo { bit_class: [-1; 6] };
        for k in 0..6 {
            if c.need & (1 << k) == 0 {
                continue;
            }
            let real: Vec<u32> = c.sets[k].iter().copied().filter(|&p| p != 0).collect();
            let has_entry = c.sets[k].contains(&0);
            if real.is_empty() {
                info.bit_class[k] = -1;
            } else {
                let cid = fp.class_of[real[0] as usize - 1];
                info.bit_class[k] = cid;
                let cl = &mut fp.classes[cid as usize];
                cl.bits_used |= 1 << k;
                if has_entry {
                    cl.has_entry = true;
                    cl.lazy = None;
                }
            }
        }
        fp.cons.insert(c.key, info);
    }
    // consumers in unreachable blocks
    for (bi, b) in func.blocks.iter().enumerate() {
        for (ii, ins) in b.insts.iter().enumerate() {
            if consumer_bits(&ins.op).is_some() && !fp.cons.contains_key(&(bi as u32, ii as u32)) {
                fp.cons.insert((bi as u32, ii as u32), ConsInfo { bit_class: [-1; 6] });
            }
        }
    }
    fp
}
