//! Whole-program view: all recompiled modules, cross-module symbol resolution, import tables.

use crate::config::Config;
use crate::image::Image;
use anyhow::{anyhow, bail, Result};
use std::collections::{BTreeMap, HashMap, HashSet};

#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct HleImport {
    pub dll: String,
    pub name: Option<String>,
    pub ordinal: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IatTarget {
    /// Recompiled export: guest address
    Guest(u32),
    /// HLE import index
    Import(u32),
}

pub struct Program {
    pub cfg: Config,
    pub images: Vec<Image>,
    /// HLE imports in thunk-index order (deduplicated)
    pub imports: Vec<HleImport>,
    /// every IAT slot (module idx, slot va) -> target
    pub iat: BTreeMap<u32, IatTarget>,
    /// IAT slot -> import description (for diagnostics / noreturn)
    pub iat_name: HashMap<u32, (String, String)>,
    pub noreturn_funcs: HashSet<u32>,
    pub noreturn_slots: HashSet<u32>,
    /// export lookup: (lower-case module name, export name) -> va
    pub export_by_name: HashMap<(String, String), u32>,
}

pub fn module_key(n: &str) -> String {
    n.to_lowercase()
}

impl Program {
    pub fn from_images(cfg: Config, images: Vec<Image>) -> Program {
        Program {
            cfg,
            images,
            imports: vec![],
            iat: BTreeMap::new(),
            iat_name: HashMap::new(),
            noreturn_funcs: HashSet::new(),
            noreturn_slots: HashSet::new(),
            export_by_name: HashMap::new(),
        }
    }

    pub fn load(cfg: Config, game_dir: &str) -> Result<Program> {
        let mut images = Vec::new();
        for m in &cfg.modules {
            let path = format!("{}/{}", game_dir.trim_end_matches('/'), m.relpath);
            let img = Image::load(&path, &m.name, &m.relpath, m.base)?;
            if let Some(h) = &m.sha256 {
                let got: String = img.sha256.iter().map(|b| format!("{b:02x}")).collect();
                if !got.eq_ignore_ascii_case(h) {
                    bail!("sha256 mismatch for {}: expected {h}, got {got}", m.name);
                }
            }
            images.push(img);
        }
        // overlap check
        for i in 0..images.len() {
            for j in i + 1..images.len() {
                let (a, b) = (&images[i], &images[j]);
                if a.base < b.base + b.size_of_image && b.base < a.base + a.size_of_image {
                    bail!("modules {} and {} overlap", a.name, b.name);
                }
            }
        }
        let mut export_by_name = HashMap::new();
        for im in &images {
            for e in &im.exports {
                if e.va != 0 {
                    if let Some(n) = &e.name {
                        export_by_name.insert((module_key(&im.name), n.clone()), e.va);
                    }
                    export_by_name.insert((module_key(&im.name), format!("ordinal:{}", e.ordinal)), e.va);
                }
            }
        }
        // IAT resolution
        let mut imports: Vec<HleImport> = Vec::new();
        let mut import_idx: HashMap<HleImport, u32> = HashMap::new();
        let mut iat = BTreeMap::new();
        let mut iat_name = HashMap::new();
        let by_mod: HashMap<String, usize> = images.iter().enumerate().map(|(i, m)| (module_key(&m.name), i)).collect();
        for im in &images {
            for s in &im.imports {
                let label = match &s.name {
                    Some(n) => n.clone(),
                    None => format!("ord{}", s.ordinal),
                };
                iat_name.insert(s.slot, (s.dll.clone(), label));
                // recompiled target?
                let mut target = None;
                if let Some(&mi) = by_mod.get(&s.dll) {
                    let tm = &images[mi];
                    let ex = tm.exports.iter().find(|e| match &s.name {
                        Some(n) => e.name.as_deref() == Some(n.as_str()),
                        None => e.ordinal == s.ordinal as u32,
                    });
                    if let Some(e) = ex {
                        if e.va != 0 {
                            target = Some(IatTarget::Guest(e.va));
                        }
                    }
                }
                // A delay import of a recompiled DLL keeps the slot's file value (the module's own delay-load stub): the first call
                // goes through __delayLoadHelper2 -> LoadLibrary -> GetProcAddress as on Windows, so the DLL's DllMain runs at load
                // time instead of never. Calls through the slot stay indirect.
                if s.delay && target.is_some() {
                    continue;
                }
                let t = match target {
                    Some(t) => t,
                    None => {
                        let key = HleImport { dll: s.dll.clone(), name: s.name.clone(), ordinal: if s.name.is_some() { 0 } else { s.ordinal } };
                        let idx = *import_idx.entry(key.clone()).or_insert_with(|| {
                            imports.push(key);
                            (imports.len() - 1) as u32
                        });
                        IatTarget::Import(idx)
                    }
                };
                iat.insert(s.slot, t);
            }
        }
        // noreturn resolution
        let mut noreturn_slots = HashSet::new();
        let nr_imp: HashSet<&String> = cfg.noreturn_imports.iter().collect();
        for im in &images {
            for s in &im.imports {
                if let Some(n) = &s.name {
                    if nr_imp.contains(&format!("{}!{}", s.dll, n)) {
                        noreturn_slots.insert(s.slot);
                    }
                }
            }
        }
        let mut noreturn_funcs = HashSet::new();
        for n in &cfg.noreturn {
            if let Some((m, e)) = n.split_once('!') {
                if let Some(&va) = export_by_name.get(&(module_key(m), e.to_string())) {
                    noreturn_funcs.insert(va);
                }
            } else if let Some(h) = n.strip_prefix("0x") {
                noreturn_funcs.insert(u32::from_str_radix(h, 16).map_err(|e| anyhow!("{e}"))?);
            }
        }
        Ok(Program { cfg, images, imports, iat, iat_name, noreturn_funcs, noreturn_slots, export_by_name })
    }

    pub fn image_of(&self, va: u32) -> Option<usize> {
        self.images.iter().position(|m| m.contains(va))
    }
}
