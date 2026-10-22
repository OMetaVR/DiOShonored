//! Non-local continuations of MSVC exception handling: addresses in the middle of a function where execution resumes after a
//! handler ran, through a jump the lifted code cannot express as a call (docs/guest_abi.md section 8, "landing pads").
//!
//! * C++ EH: `FuncInfo` tables (magic 0x1993052x) are found through the per-function handler stubs `mov eax, FuncInfo; jmp ...`.
//!   Each catch funclet returns its continuation in eax (`mov eax, imm32` ... `ret`); msvcr90's _JumpToContinuation then sets
//!   ebp = registration + 0xC, esp = [ebp-10h] and jumps there.
//! * SEH4: scope tables referenced by the prolog (`push -2; push ScopeTable` inline, or `push ScopeTable; call __SEH_prolog4`).
//!   Records with a filter have an `__except` body that _EH4_TransferToHandler jumps to with the frame's ebp; the body starts by
//!   reloading esp from [ebp-18h].
//!
//! The scan only reads the rebased image (tables hold absolute addresses at the chosen base).
use crate::image::Image;
use iced_x86::{Decoder, DecoderOptions, FlowControl, Mnemonic, OpKind, Register};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum ContKind {
    /// C++ catch continuation: esp is restored from [ebp-10h]
    Catch,
    /// SEH __except body: esp is restored from [ebp-18h]
    Except,
}

impl ContKind {
    /// ebp-relative slot holding the esp the continuation runs with
    pub fn esp_slot(self) -> i32 {
        match self {
            ContKind::Catch => -0x10,
            ContKind::Except => -0x18,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Cont {
    pub addr: u32,
    pub kind: ContKind,
    /// an address inside the owner's prolog: the push of the handler stub (C++) or of the scope table (SEH4)
    pub site: u32,
}

fn rd32(img: &Image, a: u32) -> Option<u32> {
    let r = a.checked_sub(img.base)? as usize;
    let b = img.mem.get(r..r + 4)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn decode(img: &Image, a: u32) -> Option<iced_x86::Instruction> {
    let r = a.checked_sub(img.base)? as usize;
    let bytes = img.mem.get(r..(r + 16).min(img.mem.len()))?;
    let i = Decoder::with_ip(32, bytes, a as u64, DecoderOptions::NONE).decode();
    if i.is_invalid() {
        None
    } else {
        Some(i)
    }
}

/// continuation addresses a catch funclet returns: `mov eax, imm32` reaching a `ret` on some path (bounded walk)
fn funclet_returns(img: &Image, f: u32, out: &mut BTreeSet<u32>) {
    let mut seen = BTreeSet::new();
    let mut work = vec![(f, None::<u32>)];
    while let Some((a, mut eax)) = work.pop() {
        if seen.len() > 400 || !seen.insert(a) || !img.in_code(a) {
            continue;
        }
        let Some(i) = decode(img, a) else { continue };
        if i.mnemonic() == Mnemonic::Mov && i.op_count() == 2 && i.op0_kind() == OpKind::Register && i.op0_register() == Register::EAX && i.op1_kind() == OpKind::Immediate32 {
            eax = Some(i.immediate32());
        }
        let nxt = i.next_ip() as u32;
        match i.flow_control() {
            FlowControl::Return => {
                if let Some(c) = eax {
                    if img.in_code(c) {
                        out.insert(c);
                    }
                }
            }
            FlowControl::UnconditionalBranch => work.push((i.near_branch_target() as u32, eax)),
            FlowControl::ConditionalBranch => {
                work.push((i.near_branch_target() as u32, eax));
                work.push((nxt, eax));
            }
            FlowControl::IndirectBranch | FlowControl::Interrupt | FlowControl::Exception => {}
            FlowControl::Call | FlowControl::IndirectCall => work.push((nxt, None)),
            _ => work.push((nxt, eax)),
        }
    }
}

fn scope_records(img: &Image, t: u32) -> Option<Vec<(u32, u32, u32)>> {
    let (gs, eh) = (rd32(img, t)?, rd32(img, t + 8)?);
    if !(gs == 0xFFFF_FFFE || gs < 0x8000_0000 || gs > 0xFFFF_0000) || !(eh > 0xFFFF_0000 || eh < 0x1000) {
        return None;
    }
    let mut recs = Vec::new();
    let mut r = t + 16;
    for n in 0..64u32 {
        let (enc, flt, hnd) = (rd32(img, r)?, rd32(img, r + 4)?, rd32(img, r + 8)?);
        if !img.in_code(hnd) || !(enc == 0xFFFF_FFFE || enc < n) || (flt != 0 && flt != 1 && !img.in_code(flt)) {
            break;
        }
        recs.push((enc, flt, hnd));
        r += 12;
    }
    if recs.is_empty() {
        None
    } else {
        Some(recs)
    }
}

pub fn scan(img: &Image) -> Vec<Cont> {
    let mut out: BTreeSet<Cont> = BTreeSet::new();
    let lo = img.base;
    let hi = img.base + img.mem.len() as u32;
    let byte = |a: u32| img.mem[(a - img.base) as usize];
    // where each code address is pushed as an immediate (the C++ EH prolog pushes its handler stub)
    let mut pushed_at: BTreeMap<u32, u32> = BTreeMap::new();
    for a in lo..hi.saturating_sub(16) {
        if img.in_code(a) && byte(a) == 0x68 {
            if let Some(v) = rd32(img, a + 1) {
                pushed_at.entry(v).or_insert(a);
            }
        }
    }
    for a in lo..hi.saturating_sub(16) {
        if !img.in_code(a) {
            continue;
        }
        let b0 = byte(a);
        // C++: handler stub `mov eax, FuncInfo; jmp rel32`
        if b0 == 0xB8 && byte(a + 5) == 0xE9 {
            let Some(fi) = rd32(img, a + 1) else { continue };
            let Some(magic) = rd32(img, fi) else { continue };
            if !(0x1993_0520..=0x1993_0522).contains(&magic) {
                continue;
            }
            let (Some(ntry), Some(ptry)) = (rd32(img, fi + 12), rd32(img, fi + 16)) else { continue };
            if ntry > 1000 {
                continue;
            }
            for t in 0..ntry {
                let tb = ptry + 20 * t;
                let (Some(ncatch), Some(phand)) = (rd32(img, tb + 12), rd32(img, tb + 16)) else { continue };
                for c in 0..ncatch.min(64) {
                    if let Some(h) = rd32(img, phand + 16 * c + 12) {
                        if img.in_code(h) {
                            let mut conts = BTreeSet::new();
                            funclet_returns(img, h, &mut conts);
                            // the prolog pushes the stub's start; with /GS the stub checks the frame cookie before `mov eax, FuncInfo`
                            if let Some((_, &site)) = pushed_at.range(a.saturating_sub(0x40)..=a).next_back() {
                                for k in conts {
                                    out.insert(Cont { addr: k, kind: ContKind::Catch, site });
                                }
                            }
                        }
                    }
                }
            }
        }
        // SEH4: `push -2; push ScopeTable` or `push ScopeTable; call`
        if b0 == 0x68 {
            let Some(t) = rd32(img, a + 1) else { continue };
            if t < lo || t >= hi {
                continue;
            }
            let inline_prolog = a >= lo + 2 && byte(a - 2) == 0x6A && byte(a - 1) == 0xFE;
            let helper_call = byte(a + 5) == 0xE8;
            if !(inline_prolog || helper_call) {
                continue;
            }
            if let Some(recs) = scope_records(img, t) {
                for (_, flt, hnd) in recs {
                    if flt != 0 {
                        out.insert(Cont { addr: hnd, kind: ContKind::Except, site: a });
                    }
                }
            }
        }
    }
    out.into_iter().collect()
}
