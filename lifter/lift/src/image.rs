//! Minimal PE32 loader: maps a file to a flat memory image at a chosen base (applying .reloc if the base differs from
//! the preferred base), and parses imports, delay imports and exports. No game bytes are stored anywhere except in memory.

use anyhow::{anyhow, bail, Result};
use sha2::{Digest, Sha256};

pub const SCN_EXEC: u32 = 0x2000_0000;
pub const SCN_CODE: u32 = 0x0000_0020;
pub const SCN_WRITE: u32 = 0x8000_0000;

#[derive(Clone, Debug)]
pub struct Section {
    pub name: String,
    pub va: u32, // absolute (base + rva)
    pub vsize: u32,
    pub raw_off: u32,
    pub raw_size: u32,
    pub flags: u32,
}

#[derive(Clone, Debug)]
pub struct ImportSlot {
    pub slot: u32, // absolute VA of the IAT slot
    pub dll: String, // lower-case
    pub name: Option<String>,
    pub ordinal: u16,
    pub delay: bool,
}

#[derive(Clone, Debug)]
pub struct Export {
    pub name: Option<String>,
    pub ordinal: u32,
    pub va: u32, // absolute; 0 if forwarded
    pub forward: Option<String>,
}

pub struct Image {
    pub name: String,
    pub relpath: String,
    pub sha256: [u8; 32],
    pub base: u32,
    pub preferred_base: u32,
    pub size_of_image: u32,
    pub entry: u32, // absolute, 0 if none
    pub is_dll: bool,
    pub sections: Vec<Section>,
    pub mem: Vec<u8>, // size_of_image bytes, rebased
    pub relocs: Vec<u32>, // absolute VAs of HIGHLOW fixups (sorted)
    pub imports: Vec<ImportSlot>,
    pub exports: Vec<Export>,
    pub tls_callbacks: Vec<u32>,
}

fn rd16(b: &[u8], o: usize) -> Result<u16> {
    b.get(o..o + 2).map(|s| u16::from_le_bytes([s[0], s[1]])).ok_or_else(|| anyhow!("pe: read16 oob {o:#x}"))
}
fn rd32(b: &[u8], o: usize) -> Result<u32> {
    b.get(o..o + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]])).ok_or_else(|| anyhow!("pe: read32 oob {o:#x}"))
}

fn cstr(b: &[u8], o: usize) -> String {
    let mut e = o;
    while e < b.len() && b[e] != 0 {
        e += 1;
    }
    String::from_utf8_lossy(&b[o..e]).into_owned()
}

impl Image {
    pub fn load(path: &str, name: &str, relpath: &str, base: Option<u32>) -> Result<Image> {
        let file = std::fs::read(path).map_err(|e| anyhow!("reading {path}: {e}"))?;
        let mut h = Sha256::new();
        h.update(&file);
        let sha256: [u8; 32] = h.finalize().into();
        if rd16(&file, 0)? != 0x5a4d {
            bail!("{path}: not MZ");
        }
        let pe = rd32(&file, 0x3c)? as usize;
        if rd32(&file, pe)? != 0x4550 {
            bail!("{path}: not PE");
        }
        let nsec = rd16(&file, pe + 6)? as usize;
        let opt_size = rd16(&file, pe + 20)? as usize;
        let chars = rd16(&file, pe + 22)?;
        let opt = pe + 24;
        if rd16(&file, opt)? != 0x10b {
            bail!("{path}: not PE32");
        }
        let entry_rva = rd32(&file, opt + 16)?;
        let preferred_base = rd32(&file, opt + 28)?;
        let size_of_image = rd32(&file, opt + 56)?;
        let size_of_headers = rd32(&file, opt + 60)?;
        let ndd = rd32(&file, opt + 92)? as usize;
        let dd = |i: usize| -> Result<(u32, u32)> {
            if i < ndd {
                Ok((rd32(&file, opt + 96 + i * 8)?, rd32(&file, opt + 100 + i * 8)?))
            } else {
                Ok((0, 0))
            }
        };
        let base = base.unwrap_or(preferred_base);
        let sec0 = opt + opt_size;
        let mut sections = Vec::new();
        let mut mem = vec![0u8; size_of_image as usize];
        let hn = (size_of_headers as usize).min(file.len()).min(mem.len());
        mem[..hn].copy_from_slice(&file[..hn]);
        for i in 0..nsec {
            let s = sec0 + i * 40;
            let nm = cstr(&file[s..s + 8], 0);
            let vsize = rd32(&file, s + 8)?;
            let rva = rd32(&file, s + 12)?;
            let rsize = rd32(&file, s + 16)?;
            let roff = rd32(&file, s + 20)?;
            let fl = rd32(&file, s + 36)?;
            let n = (rsize.min(if vsize == 0 { rsize } else { vsize })) as usize;
            let src = file.get(roff as usize..roff as usize + n).ok_or_else(|| anyhow!("section {nm} raw oob"))?;
            mem[rva as usize..rva as usize + n].copy_from_slice(src);
            sections.push(Section { name: nm, va: base.wrapping_add(rva), vsize: vsize.max(0), raw_off: roff, raw_size: rsize, flags: fl });
        }
        // Relocations
        let (rrva, rsz) = dd(5)?;
        let mut relocs = Vec::new();
        let delta = base.wrapping_sub(preferred_base);
        if rrva != 0 && chars & 1 == 0 {
            let mut o = rrva as usize;
            let end = o + rsz as usize;
            while o + 8 <= end {
                let page = rd32(&mem, o)?;
                let bsz = rd32(&mem, o + 4)? as usize;
                if bsz < 8 {
                    break;
                }
                let mut p = o + 8;
                while p + 2 <= o + bsz {
                    let e = rd16(&mem, p)?;
                    p += 2;
                    let ty = e >> 12;
                    let off = (e & 0xfff) as u32;
                    if ty == 3 {
                        relocs.push(base.wrapping_add(page + off));
                    } else if ty != 0 {
                        bail!("{path}: unsupported reloc type {ty}");
                    }
                }
                o += bsz;
            }
        }
        // `relocs` are read from the (not yet rebased) memory; apply delta
        if delta != 0 {
            for &va in &relocs {
                let rva = (va.wrapping_sub(base)) as usize;
                let v = rd32(&mem, rva)?.wrapping_add(delta);
                mem[rva..rva + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        relocs.sort_unstable();
        relocs.dedup();
        // Imports
        let mut imports = Vec::new();
        let (irva, _) = dd(1)?;
        if irva != 0 {
            let mut d = irva as usize;
            loop {
                let oft = rd32(&mem, d)?;
                let nm = rd32(&mem, d + 12)?;
                let ft = rd32(&mem, d + 16)?;
                if nm == 0 && ft == 0 {
                    break;
                }
                let dll = cstr(&mem, nm as usize).to_lowercase();
                let lookup = if oft != 0 { oft } else { ft } as usize;
                let mut i = 0usize;
                loop {
                    let t = rd32(&file_or(&mem), lookup + i * 4)?;
                    if t == 0 {
                        break;
                    }
                    let slot = base + ft + (i as u32) * 4;
                    if t & 0x8000_0000 != 0 {
                        imports.push(ImportSlot { slot, dll: dll.clone(), name: None, ordinal: (t & 0xffff) as u16, delay: false });
                    } else {
                        let nm = cstr(&mem, t as usize + 2);
                        imports.push(ImportSlot { slot, dll: dll.clone(), name: Some(nm), ordinal: rd16(&mem, t as usize)?, delay: false });
                    }
                    i += 1;
                }
                d += 20;
            }
        }
        // Delay imports
        let (drva, _) = dd(13)?;
        if drva != 0 {
            let mut d = drva as usize;
            loop {
                let attrs = rd32(&mem, d)?;
                let nm = rd32(&mem, d + 4)?;
                if nm == 0 {
                    break;
                }
                let rv = |v: u32| -> u32 { if attrs & 1 != 0 { v } else { v.wrapping_sub(preferred_base) } };
                let dll = cstr(&mem, rv(nm) as usize).to_lowercase();
                let iat = rv(rd32(&mem, d + 12)?);
                let int = rv(rd32(&mem, d + 16)?);
                let mut i = 0usize;
                loop {
                    let t = rd32(&mem, int as usize + i * 4)?;
                    if t == 0 {
                        break;
                    }
                    let slot = base + iat + (i as u32) * 4;
                    if t & 0x8000_0000 != 0 {
                        imports.push(ImportSlot { slot, dll: dll.clone(), name: None, ordinal: (t & 0xffff) as u16, delay: true });
                    } else {
                        let nm = cstr(&mem, rv(t) as usize + 2);
                        imports.push(ImportSlot { slot, dll: dll.clone(), name: Some(nm), ordinal: 0, delay: true });
                    }
                    i += 1;
                }
                d += 32;
            }
        }
        // Exports
        let mut exports = Vec::new();
        let (erva, esz) = dd(0)?;
        if erva != 0 {
            let e = erva as usize;
            let ord_base = rd32(&mem, e + 16)?;
            let nfunc = rd32(&mem, e + 20)? as usize;
            let nnames = rd32(&mem, e + 24)? as usize;
            let afunc = rd32(&mem, e + 28)? as usize;
            let anames = rd32(&mem, e + 32)? as usize;
            let aord = rd32(&mem, e + 36)? as usize;
            let mut names: Vec<Option<String>> = vec![None; nfunc];
            for i in 0..nnames {
                let nrva = rd32(&mem, anames + i * 4)? as usize;
                let oi = rd16(&mem, aord + i * 2)? as usize;
                if oi < nfunc {
                    names[oi] = Some(cstr(&mem, nrva));
                }
            }
            for i in 0..nfunc {
                let f = rd32(&mem, afunc + i * 4)?;
                if f == 0 {
                    continue;
                }
                if f >= erva && f < erva + esz {
                    exports.push(Export { name: names[i].clone(), ordinal: ord_base + i as u32, va: 0, forward: Some(cstr(&mem, f as usize)) });
                } else {
                    exports.push(Export { name: names[i].clone(), ordinal: ord_base + i as u32, va: base + f, forward: None });
                }
            }
        }
        // TLS callbacks
        let mut tls_callbacks = Vec::new();
        let (trva, _) = dd(9)?;
        if trva != 0 {
            let cb = rd32(&mem, trva as usize + 12)?; // AddressOfCallBacks (VA at preferred base, rebased)
            if cb != 0 {
                let mut p = cb.wrapping_sub(base) as usize;
                while let Ok(v) = rd32(&mem, p) {
                    if v == 0 {
                        break;
                    }
                    tls_callbacks.push(v);
                    p += 4;
                }
            }
        }
        Ok(Image {
            name: name.to_string(),
            relpath: relpath.to_string(),
            sha256,
            base,
            preferred_base,
            size_of_image,
            entry: if entry_rva != 0 { base + entry_rva } else { 0 },
            is_dll: chars & 0x2000 != 0,
            sections,
            mem,
            relocs,
            imports,
            exports,
            tls_callbacks,
        })
    }

    /// A synthetic single-section code image (tests): `bytes` placed at `base`, executable.
    pub fn from_blob(name: &str, base: u32, bytes: &[u8]) -> Image {
        let size = ((bytes.len() + 0xfff) & !0xfff) as u32 + 0x1000;
        let mut mem = vec![0u8; size as usize];
        mem[..bytes.len()].copy_from_slice(bytes);
        Image {
            name: name.to_string(),
            relpath: name.to_string(),
            sha256: [0; 32],
            base,
            preferred_base: base,
            size_of_image: size,
            entry: 0,
            is_dll: false,
            sections: vec![Section { name: ".text".into(), va: base, vsize: size, raw_off: 0, raw_size: size, flags: SCN_EXEC | SCN_CODE }],
            mem,
            relocs: vec![],
            imports: vec![],
            exports: vec![],
            tls_callbacks: vec![],
        }
    }

    #[inline]
    pub fn contains(&self, va: u32) -> bool {
        va >= self.base && va - self.base < self.size_of_image
    }
    #[inline]
    pub fn rva(&self, va: u32) -> usize {
        (va - self.base) as usize
    }
    pub fn read32(&self, va: u32) -> Option<u32> {
        if !self.contains(va) {
            return None;
        }
        let o = self.rva(va);
        self.mem.get(o..o + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    pub fn read8(&self, va: u32) -> Option<u8> {
        if !self.contains(va) {
            return None;
        }
        self.mem.get(self.rva(va)).copied()
    }
    pub fn bytes(&self, va: u32, n: usize) -> Option<&[u8]> {
        if !self.contains(va) {
            return None;
        }
        let o = self.rva(va);
        self.mem.get(o..(o + n).min(self.mem.len()))
    }
    pub fn section_of(&self, va: u32) -> Option<&Section> {
        self.sections.iter().find(|s| va >= s.va && va < s.va + s.vsize.max(s.raw_size))
    }
    pub fn is_exec_section(&self, s: &Section) -> bool {
        s.flags & (SCN_EXEC | SCN_CODE) != 0
    }
    /// Executable ranges [start, end) clipped to what is backed by file bytes.
    pub fn code_ranges(&self) -> Vec<(u32, u32)> {
        self.sections
            .iter()
            .filter(|s| self.is_exec_section(s))
            .map(|s| (s.va, s.va + s.vsize.min(s.raw_size.max(s.vsize))))
            .collect()
    }
    pub fn in_code(&self, va: u32) -> bool {
        self.sections.iter().any(|s| self.is_exec_section(s) && va >= s.va && va < s.va + s.vsize)
    }
}

// helper to keep the borrow checker simple in the import loop
fn file_or(m: &[u8]) -> &[u8] {
    m
}
