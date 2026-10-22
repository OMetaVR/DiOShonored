//! Lifter configuration (TOML). Contains addresses, names and hashes only; never game bytes.

use anyhow::{Context, Result};
use serde::Deserialize;

#[derive(Debug, Deserialize, Clone)]
#[serde(deny_unknown_fields)]
pub struct ModuleCfg {
    pub name: String,
    pub relpath: String,
    /// Chosen guest base; defaults to the preferred base from the PE header.
    pub base: Option<u32>,
    /// Expected SHA-256 of the file (hex); checked when present.
    pub sha256: Option<String>,
    /// Extra function entries (absolute guest addresses).
    #[serde(default)]
    pub entries: Vec<u32>,
    /// Addresses that must NOT become entries (labels inside functions that happen to be relocated).
    #[serde(default)]
    pub not_entries: Vec<u32>,
    /// Ranges [start, end) of code-section bytes that are data / must never be decoded.
    #[serde(default)]
    pub data_ranges: Vec<[u32; 2]>,
}

#[derive(Debug, Deserialize, Clone)]
#[serde(deny_unknown_fields)]
pub struct OverrideCfg {
    pub module: String,
    /// Export name (or "ordinal:N"); one of `export` / `addr` must be present.
    pub export: Option<String>,
    pub addr: Option<u32>,
    /// Host symbol; must be `ovr_<name>`.
    pub host: String,
    /// "guestfn" (default): host symbol has the GuestFn signature and replaces the function body.
    /// "x87_1": `double host(double st0)` -> result pushed (st0 popped).
    /// "x87_2": `double host(double st1, double st0)` -> result pushed (both popped).
    /// "x87_ftol32" / "x87_ftol64": compiler float->int helpers (_ftol2_sse / _ftol2) that take st0 from the caller; lowered inline at
    /// the call site to a truncating store (eax, or edx:eax) and a pop. `host` is unused (by convention "inline").
    #[serde(default = "default_kind")]
    pub kind: String,
    /// bytes popped by the host function (cdecl = 0); absent = dynamic (read from the returned GuestRet)
    pub pop: Option<u16>,
    /// free-form note (e.g. the helper's name for an `addr` override)
    pub why: Option<String>,
}
fn default_kind() -> String {
    "guestfn".into()
}

#[derive(Debug, Deserialize, Clone)]
pub struct HookCfg {
    pub module: String,
    pub addr: u32,
    pub host: String,
    /// bytes popped by the replacement (default 0)
    #[serde(default)]
    pub pop: u16,
    /// free-text reason (documentation only)
    pub why: Option<String>,
}

#[derive(Debug, Deserialize, Clone, Default)]
#[serde(deny_unknown_fields)]
pub struct Config {
    #[serde(rename = "module", default)]
    pub modules: Vec<ModuleCfg>,
    #[serde(rename = "override", default)]
    pub overrides: Vec<OverrideCfg>,
    #[serde(rename = "hook", default)]
    pub hooks: Vec<HookCfg>,
    /// Functions that never return: "module!export" or numeric address strings like "0x12345678".
    #[serde(default)]
    pub noreturn: Vec<String>,
    /// Imports (dll!name) that never return.
    #[serde(default)]
    pub noreturn_imports: Vec<String>,
    /// Functions (absolute addr) whose indirect calls/jumps pass these registers through the side channel.
    #[serde(default)]
    pub ext_pass: Vec<ExtPassCfg>,
    #[serde(default)]
    pub shard_instructions: Option<usize>,
    #[serde(default)]
    pub base_register: bool,
}

#[derive(Debug, Deserialize, Clone)]
pub struct ExtPassCfg {
    pub module: String,
    pub addr: u32,
    pub regs: Vec<String>,
}

impl Config {
    pub fn load(path: &str) -> Result<Config> {
        let s = std::fs::read_to_string(path).with_context(|| format!("reading config {path}"))?;
        let c: Config = toml::from_str(&s).with_context(|| format!("parsing config {path}"))?;
        Ok(c)
    }
}
