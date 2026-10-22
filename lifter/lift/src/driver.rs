//! The `lift` command: analysis, emission, output tree and reports.

use crate::ctx::LiftCtx;
use crate::genh;
use crate::output::*;
use crate::pipeline::{self, FnOut};
use anyhow::Result;
use rayon::prelude::*;
use std::fs;
use std::path::Path;

pub fn run_lift(ctx: &mut LiftCtx, outdir: &Path, args: &[String], log: &mut dyn FnMut(String)) -> Result<()> {
    let shard_instr: usize = args.iter().position(|a| a == "--shard-instr").and_then(|i| args.get(i + 1)).and_then(|s| s.parse().ok()).or(ctx.prog.cfg.shard_instructions).unwrap_or(40_000);
    let only: Option<Vec<u32>> = args.iter().position(|a| a == "--only").and_then(|i| args.get(i + 1)).map(|s| s.split(',').filter_map(|x| u32::from_str_radix(x.trim_start_matches("0x"), 16).ok()).collect());
    let trace = args.iter().any(|a| a == "--trace");
    let an = pipeline::analyze_program(ctx, log);
    log(format!("analysis done: {} functions, {} rounds", an.facts.len(), an.rounds));
    fs::create_dir_all(outdir)?;
    let c: &LiftCtx = ctx;
    let mut list: Vec<(u32, usize)> = c.all_funcs().into_iter().filter(|(a, _)| !pipeline::skip_body(c, *a)).collect();
    if let Some(o) = &only {
        list.retain(|(a, _)| o.contains(a));
    }
    let outs: Vec<FnOut> = list.par_iter().map(|&(a, mi)| pipeline::emit_one(c, &an, mi, a, trace)).collect();
    log(format!("emitted {} functions", outs.len()));
    // reports
    let rep = outdir.join("report");
    fs::create_dir_all(&rep)?;
    {
        let mut t = String::from("addr\tmodule\tn_x86\tcats\text_in\text_out\tpop\tn_unsupported\tn_traps\n");
        let mut keys: Vec<&u32> = an.facts.keys().collect();
        keys.sort();
        for a in keys {
            let f = &an.facts[a];
            let sg = &an.sigs[a];
            let cats: Vec<&str> = (0..pipeline::CAT_NAMES.len()).filter(|i| f.cats & (1 << i) != 0).map(|i| pipeline::CAT_NAMES[i]).collect();
            t.push_str(&format!("{:08x}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", a, f.module, f.n_x86, cats.join(","), sg.ext_in, sg.ext_out, sg.pop.map(|p| p as i32).unwrap_or(-1), f.n_unsupported, f.n_traps));
        }
        fs::write(rep.join("functions.tsv"), t)?;
        let mut x = String::from("func\taddr\tkind\n");
        let mut keys: Vec<&u32> = an.facts.keys().collect();
        keys.sort();
        for a in keys {
            for (at, k) in &an.facts[a].x87_anom_list {
                x.push_str(&format!("{:08x}\t{:08x}\t{}\n", a, at, k));
            }
        }
        fs::write(rep.join("x87.tsv"), x)?;
        let mut u = String::from("addr\treason\n");
        let mut tr = String::from("addr\twhat\n");
        for o in &outs {
            for (a, r) in &o.unsupported {
                u.push_str(&format!("{:08x}\t{}\n", a, r));
            }
            for (a, r) in &o.traps {
                tr.push_str(&format!("{:08x}\t{}\n", a, r));
            }
        }
        let mut iat = String::new();
        for (slot, t) in &c.prog.iat {
            match t {
                crate::program::IatTarget::Guest(a) => iat.push_str(&format!("{:08x}\t0\t{:08x}\n", slot, a)),
                crate::program::IatTarget::Import(i) => iat.push_str(&format!("{:08x}\t1\t{}\n", slot, i)),
            }
        }
        fs::write(rep.join("iat.tsv"), iat)?;
        fs::write(rep.join("unsupported.tsv"), u)?;
        fs::write(rep.join("traps.tsv"), tr)?;
    }
    let simd = String::new();
    fs::write(outdir.join("gen.h"), genh::gen_h(&simd))?;
    if trace {
        fs::write(outdir.join("trace.c"), "#include \"gen.h\"\nextern int dprintf(int, const char *, ...);\nextern char *getenv(const char *);\nvoid lx_trace_enter(uint32_t a, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {\n    static int on = -1;\n    if (on < 0) on = getenv(\"LX_TRACE\") != 0;\n    if (on) dprintf(2, \"TR %08x esp=%08x eax=%08x ecx=%08x edx=%08x\\n\", a, esp, eax, ecx, edx);\n}\n")?;
    }
    let shards = make_shards(&outs, c.prog.images.len(), shard_instr);
    for m in &c.prog.images {
        fs::create_dir_all(outdir.join(&m.name))?;
    }
    for sh in &shards {
        let p = outdir.join(&c.prog.images[sh.module].name).join(format!("shard_{:04}.c", sh.index));
        write_shard(c, &an, &outs, sh, &p)?;
    }
    log(format!("wrote {} shards", shards.len()));
    // weak default definitions of config hook symbols (the runtime may provide real ones)
    {
        let mut t = String::from("/* generated: weak defaults for lifter config hooks */\n#include \"gen.h\"\n");
        for h in &c.prog.cfg.hooks {
            t.push_str(&format!("__attribute__((weak)) GuestRet {}(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {{ (void)esp; (void)ecx; (void)eax; return guest_ret(0, edx, {}u); }}\n", h.host, h.pop));
        }
        fs::write(outdir.join("lx_hooks.c"), t)?;
    }
    write_imports_json(c, args, outdir)?;
    validate_ranges(c)?;
    write_dispatch(c, &an, outdir)?;
    write_program(c, outdir)?;
    log("done".into());
    Ok(())
}

/// imports.json: every HLE import with its calling convention / stack-argument size when a signature source knows it
/// (runtime/hle/sigs_generated.inc: {dll, name, ordinal, conv S|C|V, pop_bytes, ...}).
fn write_imports_json(c: &LiftCtx, args: &[String], outdir: &Path) -> Result<()> {
    let sigs_path = args.iter().position(|a| a == "--sigs").and_then(|i| args.get(i + 1)).cloned().unwrap_or_else(|| "../runtime/hle/sigs_generated.inc".to_string());
    let mut sig: std::collections::HashMap<(String, String), (char, u32)> = std::collections::HashMap::new();
    if let Ok(txt) = fs::read_to_string(&sigs_path) {
        for l in txt.lines() {
            let l = l.trim();
            if !l.starts_with("{\"") {
                continue;
            }
            let f: Vec<&str> = l.trim_start_matches('{').trim_end_matches("},").split(',').map(|x| x.trim()).collect();
            if f.len() >= 5 {
                let dll = f[0].trim_matches('"').to_lowercase();
                let name = f[1].trim_matches('"').to_string();
                let conv = f[3].trim_matches('\'').chars().next().unwrap_or('?');
                let pop: u32 = f[4].parse().unwrap_or(0);
                sig.insert((dll, name), (conv, pop));
            }
        }
    }
    let mut out = String::from("[\n");
    let mut known = 0;
    for (i, im) in c.prog.imports.iter().enumerate() {
        let (conv, pop) = match &im.name {
            Some(n) => sig.get(&(im.dll.clone(), n.clone())).copied().unwrap_or(('?', 0)),
            None => ('?', 0),
        };
        let ck = conv != '?';
        if ck {
            known += 1;
        }
        let cname = match conv {
            'S' => "stdcall",
            'C' => "cdecl",
            'V' => "cdecl_varargs",
            _ => "unknown",
        };
        out.push_str(&format!(
            "  {{\"index\": {}, \"dll\": \"{}\", \"name\": {}, \"ordinal\": {}, \"conv_known\": {}, \"conv\": \"{}\", \"arg_count_known\": {}, \"arg_bytes\": {}}}{}\n",
            i,
            im.dll,
            im.name.as_ref().map(|n| format!("\"{}\"", n)).unwrap_or("null".into()),
            im.ordinal,
            ck,
            cname,
            ck && conv != 'V',
            if ck && conv != 'V' { pop as i64 } else { -1 },
            if i + 1 < c.prog.imports.len() { "," } else { "" }
        ));
    }
    out.push_str("]\n");
    fs::write(outdir.join("imports.json"), out)?;
    eprintln!("imports.json: {} HLE imports, {} with a known calling convention", c.prog.imports.len(), known);
    Ok(())
}

/// Module ranges must not overlap each other, the null guard [0, 64K) or the thunk/sentinel region [0xFFEF0000, 4G).
fn validate_ranges(c: &LiftCtx) -> Result<()> {
    for m in &c.prog.images {
        let end = m.base as u64 + m.size_of_image as u64;
        if m.base < 0x10000 || end > 0xFFEF_0000u64 {
            anyhow::bail!("module {} [{:#x}, {:#x}) overlaps a reserved region", m.name, m.base, end);
        }
    }
    Ok(())
}
