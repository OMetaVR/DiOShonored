use anyhow::{anyhow, Result};
use lift::config::Config;
use lift::ctx::LiftCtx;
use lift::pipeline;
use lift::program::Program;
use rayon::prelude::*;
use std::path::Path;

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: lifter lift --config <toml> --game <dir> --out <dir> [--shard-instr N] [--only 0xADDR,...]");
        std::process::exit(2);
    }
    let cmd = args[1].as_str();
    let t0 = std::time::Instant::now();
    let mut log = |s: String| eprintln!("[{:7.1}s] {s}", t0.elapsed().as_secs_f32());
    if cmd == "lift-blob" {
        // synthetic single-section module for the differential tests
        let parse = |s: &str| u32::from_str_radix(s.trim_start_matches("0x"), 16).unwrap();
        let blob = std::fs::read(arg(&args, "--blob").ok_or_else(|| anyhow!("--blob"))?)?;
        let base = parse(&arg(&args, "--base").unwrap_or("0x50000000".into()));
        let entries: Vec<u32> = arg(&args, "--entries").unwrap_or_default().split(',').filter(|x| !x.is_empty()).map(parse).collect();
        let out = arg(&args, "--out").ok_or_else(|| anyhow!("--out"))?;
        let mut cfg = Config::default();
        cfg.modules.push(lift::config::ModuleCfg { name: "blob".into(), relpath: "blob".into(), base: Some(base), sha256: None, entries, not_entries: vec![], data_ranges: vec![] });
        let img = lift::image::Image::from_blob("blob", base, &blob);
        let prog = Program::from_images(cfg, vec![img]);
        let mut ctx = LiftCtx::build(prog, &mut log);
        lift::driver::run_lift(&mut ctx, Path::new(&out), &args, &mut log)?;
        return Ok(());
    }
    let cfg_path = arg(&args, "--config").ok_or_else(|| anyhow!("--config required"))?;
    let game = arg(&args, "--game").ok_or_else(|| anyhow!("--game required"))?;
    let cfg = Config::load(&cfg_path)?;
    let prog = Program::load(cfg, &game)?;
    let mut ctx = LiftCtx::build(prog, &mut log);
    match cmd {
        "lift" => {
            let out = arg(&args, "--out").ok_or_else(|| anyhow!("--out required"))?;
            lift::driver::run_lift(&mut ctx, Path::new(&out), &args, &mut log)?;
        }
        "stats" => {
            let an = pipeline::analyze_program(&mut ctx, &mut log);
            log(format!("functions with facts: {}", an.facts.len()));
            let n: u64 = an.facts.values().map(|f| f.n_x86 as u64).sum();
            let ok = an.facts.values().filter(|f| f.stack_ok).count();
            let ei = an.sigs.values().filter(|s| s.ext_in != 0).count();
            let eo = an.sigs.values().filter(|s| s.ext_out & 15 != 0).count();
            let ec = an.sigs.values().filter(|s| s.ext_out & 16 != 0).count();
            let unk = an.facts.values().filter(|f| f.unknown_mask != 0).count();
            let ra = an.sigs.values().filter(|s| s.ret_addr).count();
            let unsup: u32 = an.facts.values().map(|f| f.n_unsupported).sum();
            let inv: u32 = an.facts.values().map(|f| f.n_invalid).sum();
            let tr: u32 = an.facts.values().map(|f| f.n_traps).sum();
            let ind: u32 = an.facts.values().map(|f| f.n_indirect_calls).sum();
            let edi: u32 = an.facts.values().map(|f| f.ecx_demand_indirect).sum();
            let bt: u32 = an.facts.values().map(|f| f.bad_tails).sum();
            let dyn_pop = an.sigs.values().filter(|s| s.pop.is_none()).count();
            for mi in 0..ctx.prog.images.len() {
                let fs: Vec<_> = an.facts.iter().filter(|(_, f)| f.module == mi).collect();
                let c = |g: &dyn Fn(&lift::emit::Sig) -> bool| fs.iter().filter(|(a, _)| g(&an.sigs[a])).count();
                log(format!("{}: funcs {} stack_ok {} ext_in {} ext_out {} ecx_out {} dyn-pop {}", ctx.prog.images[mi].name, fs.len(), fs.iter().filter(|(_, f)| f.stack_ok).count(), c(&|s| s.ext_in != 0), c(&|s| s.ext_out & 15 != 0), c(&|s| s.ext_out & 16 != 0), c(&|s| s.pop.is_none())));
            }
            log(format!("x86 instrs {n}; stack_ok {ok}; ext_in {ei}; ext_out {eo}; ecx_out {ec}; unknown_mask {unk}; ret_addr {ra}; unsupported {unsup}; invalid {inv}; traps {tr}; indirect calls {ind}; ecx-demand-indirect {edi}; bad tails {bt}; dyn-pop funcs {dyn_pop}"));
        }
        "dump" => {
            let an = pipeline::analyze_program(&mut ctx, &mut |_| {});
            let mut list = arg(&args, "--addr").unwrap_or_default();
            if list == "ecxind" {
                let mut v: Vec<(u32, u32)> = an.facts.iter().filter(|(_, f)| f.ecx_demand_indirect > 0).map(|(a, f)| (f.n_x86, *a)).collect();
                v.sort();
                list = v.iter().take(4).map(|x| format!("{:x}", x.1)).collect::<Vec<_>>().join(",");
            }
            if list == "modroot" {
                let mut v: Vec<(u32, u32)> = an.facts.iter().filter(|(_, f)| f.mod_out != 0).map(|(a, f)| (f.n_x86, *a)).collect();
                v.sort();
                println!("mod roots: {}", v.len());
                list = v.iter().skip(40).step_by(300).take(10).map(|x| format!("{:x}", x.1)).collect::<Vec<_>>().join(",");
            }
            if let Some(w) = list.strip_prefix("ecxwho:") {
                let t = u32::from_str_radix(w, 16)?;
                let v: Vec<String> = an.facts.iter().filter(|(_, f)| f.ecx_demand.contains(&t)).take(3).map(|(a, _)| format!("{a:x}")).collect();
                println!("callers demanding: {v:?}");
                list = v.join(",");
            }
            if list == "dynroot" {
                let mut cc: std::collections::HashMap<u32,u32> = Default::default();
                for f in an.facts.values() { for c in &f.calls { *cc.entry(*c).or_default() += 1; } }
                let mut v: Vec<(u32, u32)> = an.facts.iter().filter(|(_, f)| f.pop_dynamic && !f.calls.iter().any(|c| an.sigs.get(c).map_or(false, |s| s.pop.is_none())) && !f.tails.iter().any(|t| matches!(t, ir::CallTarget::Direct(c) if an.sigs.get(c).map_or(false, |s| s.pop.is_none())))).map(|(a, _)| (cc.get(a).copied().unwrap_or(0), *a)).collect();
                v.sort(); v.reverse();
                println!("dyn roots: {} top: {:x?}", v.len(), &v[..12.min(v.len())]);
                list = v.iter().take(4).map(|x| format!("{:x}", x.1)).collect::<Vec<_>>().join(",");
            }
            if list == "dynpop" {
                let mut v: Vec<(u32, u32)> = an.sigs.iter().filter(|(a, s)| s.pop.is_none() && an.facts[a].module == 2).map(|(a, _)| (an.facts[a].n_x86, *a)).collect();
                v.sort();
                println!("dynpop: {}", v.len());
                list = v.iter().skip(30).step_by(1500).take(8).map(|x| format!("{:x}", x.1)).collect::<Vec<_>>().join(",");
            }
            if list == "extroot" {
                let mut v: Vec<(u32, u32)> = an.sigs.iter().filter(|(a, s)| s.ext_in != 0 && !an.facts[a].calls.iter().any(|c| an.sigs.get(c).map_or(false, |x| x.ext_in != 0)) && !an.facts[a].tails.iter().any(|t| matches!(t, ir::CallTarget::Direct(c) if an.sigs.get(c).map_or(false, |x| x.ext_in != 0)))).map(|(a, _)| (an.facts[a].n_x86, *a)).collect();
                v.sort();
                println!("roots: {}", v.len());
                let mut cc: std::collections::HashMap<u32,u32> = Default::default();
                for f in an.facts.values() { for c in &f.calls { *cc.entry(*c).or_default() += 1; } }
                let mut v2: Vec<(u32,u32)> = v.iter().map(|x| (cc.get(&x.1).copied().unwrap_or(0), x.1)).collect();
                v2.sort(); v2.reverse();
                println!("top callers: {:x?}", &v2[..10.min(v2.len())]);
                v = v2.iter().map(|x| (0, x.1)).collect();
                list = v.iter().skip(60).step_by(200).take(24).map(|x| format!("{:x}", x.1)).collect::<Vec<_>>().join(",");
            }
            if list == "extin" || list == "extout" {
                let mut v: Vec<(u32, u32)> = an.sigs.iter().filter(|(_, s)| if list == "extin" { s.ext_in != 0 } else { s.ext_out & 15 != 0 }).map(|(a, _)| (an.facts[a].n_x86, *a)).collect();
                v.sort();
                list = v.iter().take(8).map(|x| format!("{:x}", x.1)).collect::<Vec<_>>().join(",");
                println!("candidates: {list}");
            }
            for a in list.split(',') {
                let a = u32::from_str_radix(a.trim_start_matches("0x"), 16)?;
                let mi = ctx.entry_module[&a];
                let il = an.inline.get(&a).cloned().unwrap_or_default();
                let (built, abs, xi) = pipeline::analyze_function(&ctx, &an.sigs, mi, a, &il, true);
                for ((bi, ii), l) in lift::liveness::live_after_calls(&built.func, &|_| true).0 {
                    if l & 2 != 0 { println!("ECX LIVE after call blk {bi} idx {ii}: {:?}", built.func.blocks[bi as usize].insts[ii as usize].op); }
                }
                println!("=== {a:#x} sig={:?}\nuse_sites={:?}\nfacts={:?}\nabs={:?}\nx87={:?}", an.sigs.get(&a), abs.use_sites, an.facts.get(&a), abs, xi);
                for (bi, b) in built.func.blocks.iter().enumerate() {
                    println!("-- block {bi} @{:#x}", b.addr);
                    for i in &b.insts {
                        println!("   {:#x}: {:?}", i.addr, i.op);
                    }
                    println!("   term: {:?}", b.term);
                }
                println!("unsupported: {:?}", built.unsupported);
            }
        }
        _ => return Err(anyhow!("unknown command {cmd}")),
    }
    let _ = (|| ());
    let _: Vec<u32> = Vec::<u32>::new().par_iter().copied().collect();
    Ok(())
}
