#!/usr/bin/env python3
"""Generate runtime/hle/sigs_generated.inc: calling convention + stack-arg size for every import.

Inputs (all factual, no game bytes):
  third_party/wine-specs/*.spec     Wine .spec files (name, calling convention, argument types)
  tools/analysis/out/api_surface.json   the imports of the recompiled modules (+ category)
  tools/analysis/out/api_called_not_in_static_imports.txt   functions resolved dynamically at run time
  runtime/tools/manual_sigs.txt     entries for DLLs that have no Wine spec (steam_api, libcurl, cudart, ...)

Run:  python3 -I runtime/tools/gen_sigs.py            (from the repo root; paths are relative to the script)
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SPECS = os.path.join(ROOT, "third_party", "wine-specs")
OUT = os.path.join(ROOT, "runtime", "hle", "sigs_generated.inc")
REPORT = os.path.join(ROOT, "runtime", "hle", "sigs_report.txt")

# spec file -> host dll name(s). The first spec listed for a dll wins when a name appears in several.
SPEC_MAP = {
    "kernel32": ["kernel32.dll", ["kernel32", "kernelbase"]],
    "user32": ["user32.dll", ["user32"]],
    "gdi32": ["gdi32.dll", ["gdi32"]],
    "advapi32": ["advapi32.dll", ["advapi32", "kernelbase"]],
    "ole32": ["ole32.dll", ["ole32", "combase"]],
    "oleaut32": ["oleaut32.dll", ["oleaut32"]],
    "winmm": ["winmm.dll", ["winmm"]],
    "wsock32": ["wsock32.dll", ["wsock32", "ws2_32"]],
    "ws2_32": ["ws2_32.dll", ["ws2_32"]],
    "shell32": ["shell32.dll", ["shell32"]],
    "shlwapi": ["shlwapi.dll", ["shlwapi"]],
    "xinput1_3": ["xinput1_3.dll", ["xinput1_3"]],
    "dinput8": ["dinput8.dll", ["dinput8"]],
    "d3d9": ["d3d9.dll", ["d3d9"]],
    "iphlpapi": ["iphlpapi.dll", ["iphlpapi"]],
    "powrprof": ["powrprof.dll", ["powrprof"]],
    "msvcr90": ["msvcr90.dll", ["msvcr90", "msvcrt"]],
    "msvcr80": ["msvcr80.dll", ["msvcr90", "msvcrt"]],
    "msvcp90": ["msvcp90.dll", ["msvcp90"]],
    "ntdll": ["ntdll.dll", ["ntdll"]],
    "dbghelp": ["dbghelp.dll", ["dbghelp"]],
    "dsound": ["dsound.dll", ["dsound"]],
    "psapi": ["psapi.dll", ["psapi", "kernelbase"]],
    "version": ["version.dll", ["version"]],
    "faultrep": ["faultrep.dll", ["faultrep"]],
    "dwmapi": ["dwmapi.dll", ["dwmapi"]],
    "imm32": ["imm32.dll", ["imm32"]],
    "comctl32": ["comctl32.dll", ["comctl32"]],
    "secur32": ["secur32.dll", ["secur32"]],
}
# DLLs whose full spec is dumped into the table (others: only names that are needed)
FULL = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "winmm.dll",
        "wsock32.dll", "ws2_32.dll", "shell32.dll", "xinput1_3.dll", "dinput8.dll", "d3d9.dll", "iphlpapi.dll",
        "powrprof.dll", "dsound.dll", "version.dll", "shlwapi.dll"}

LINE = re.compile(r'^\s*(@|\d+)\s+(\w+)\s*((?:-\S+\s+)*)([^\s(]+)\s*(?:\(([^)]*)\))?\s*(\S+)?\s*$')

def slots(types):
    n = 0
    for t in types.split():
        n += 8 if t in ("int64", "double") else 4
    return n

def parse_spec(path):
    res = {}
    if not os.path.exists(path):
        return res
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        m = LINE.match(line)
        if not m:
            continue
        ordn, typ, flags, name, args, target = m.groups()
        if typ not in ("stdcall", "cdecl", "varargs", "thiscall", "stub", "extern", "forward"):
            continue
        arch = None
        for f in flags.split():
            if f.startswith("-arch="):
                arch = f[6:].split(",")
        if arch is not None and not ({"i386", "win32"} & set(arch)):
            continue
        if "-noname" in flags.split():
            continue
        spec_i386 = arch is not None and "i386" in arch
        ordinal = int(ordn) if ordn != "@" else 0
        if typ == "forward":
            # "@ forward Name target.Name"  -> regex captured name; target in args/target
            res.setdefault(name, dict(conv="?", pop=-1, ordinal=ordinal, forward=(args or target or "").strip(), i386=spec_i386))
            continue
        if typ == "stub" or typ == "extern":
            e = dict(conv="?", pop=-1, ordinal=ordinal, forward=None, i386=spec_i386)
        else:
            a = args or ""
            if typ == "stdcall":
                e = dict(conv="S", pop=slots(a), ordinal=ordinal, forward=None, i386=spec_i386)
            elif typ == "cdecl":
                e = dict(conv="C", pop=0, ordinal=ordinal, forward=None, i386=spec_i386)
            elif typ == "varargs":
                e = dict(conv="V", pop=0, ordinal=ordinal, forward=None, i386=spec_i386)
            else:  # thiscall: first arg in ecx, callee pops the rest
                tl = a.split()
                e = dict(conv="T", pop=slots(" ".join(tl[1:])) if tl else 0, ordinal=ordinal, forward=None, i386=spec_i386)
        old = res.get(name)
        if old is None or (e["i386"] and not old["i386"]) or (old["conv"] == "?" and e["conv"] != "?"):
            res[name] = e
    return res

def main():
    surf = json.load(open(os.path.join(ROOT, "tools", "analysis", "out", "api_surface.json")))
    needed = {}   # (dll, name) -> category
    for x in surf:
        needed[(x["dll"].lower(), x["function"])] = x["category"]
    dyn = set()
    p = os.path.join(ROOT, "tools", "analysis", "out", "api_called_not_in_static_imports.txt")
    if os.path.exists(p):
        for l in open(p):
            l = l.split("\t")[0].strip()
            if "!" in l:
                d, f = l.split("!", 1)
                dyn.add((d.lower(), f))
    # extra functions the runtime knows are resolved dynamically (docs/api_surface.md section 4 plus CRT startup)
    for f in ["FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue", "EncodePointer", "DecodePointer",
              "IsProcessorFeaturePresent", "InitializeCriticalSectionAndSpinCount", "InitializeCriticalSectionEx",
              "IsWow64Process", "GetNativeSystemInfo", "SetThreadStackGuarantee", "GetLogicalProcessorInformation",
              "GetTickCount64", "CreateTimerQueue", "SetThreadDescription", "AddDllDirectory", "LoadLibraryExA",
              "LoadLibraryExW", "GetModuleHandleExW", "GetCurrentProcessorNumber", "InitOnceExecuteOnce",
              "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive", "TryAcquireSRWLockExclusive",
              "InitializeSRWLock", "AcquireSRWLockShared", "ReleaseSRWLockShared", "SleepConditionVariableCS",
              "WakeConditionVariable", "WakeAllConditionVariable", "InitializeConditionVariable",
              "GetSystemTimePreciseAsFileTime", "QueryPerformanceCounter", "GetFileAttributesExW",
              "FindFirstFileExW", "SetFilePointerEx", "GetFileSizeEx", "CreateWaitableTimerW", "SetWaitableTimer",
              "CancelWaitableTimer", "GetThreadPriority", "GetProcessId", "GetCurrentThread", "GetStdHandle",
              "CreateIoCompletionPort", "GetOverlappedResult", "CancelIo", "OpenProcess", "TlsAlloc",
              "VirtualAllocEx", "GlobalFree", "GlobalSize", "GlobalReAlloc", "LocalReAlloc", "LocalSize", "LocalLock",
              "LocalUnlock", "GlobalMemoryStatus", "MulDiv", "lstrcpyA", "lstrcpyW", "lstrcpynA", "lstrcpynW",
              "lstrcatA", "lstrcatW", "lstrcmpA", "lstrcmpW", "lstrcmpiA", "lstrcmpiW", "GetThreadContext",
              "GetCurrentProcessorNumberEx", "GetThreadTimes", "GetProcessTimes", "GetSystemTimeAdjustment",
              "FormatMessageA", "GetModuleHandleExA", "WerRegisterFile", "OutputDebugStringA", "SetEvent"]:
        dyn.add(("kernel32.dll", f))
    for f in ["NtQueryInformationThread", "NtQueryInformationProcess", "RtlGetVersion", "RtlUnwind",
              "NtQuerySystemInformation", "RtlAddFunctionTable", "RtlCaptureContext", "NtSetInformationThread"]:
        dyn.add(("ntdll.dll", f))

    out = {}          # (dll,name) -> dict(conv,pop,ordinal,default,needed,src)
    report = []
    # 1. Wine specs
    spec_cache = {}
    def spec(n):
        if n not in spec_cache:
            spec_cache[n] = parse_spec(os.path.join(SPECS, n + ".spec"))
        return spec_cache[n]
    for key, (dll, files) in SPEC_MAP.items():
        merged = {}
        primary = set()
        for i, f in enumerate(files):
            for name, e in spec(f).items():
                if name not in merged:
                    merged[name] = e
                    if i == 0:
                        primary.add(name)
        for name, e in merged.items():
            e = dict(e)
            if e["conv"] == "?" and e.get("forward"):
                tgt = e["forward"].split(".", 1)
                if len(tgt) == 2:
                    t = spec(tgt[0].lower()).get(tgt[1])
                    if t and t["conv"] != "?":
                        e = dict(t, ordinal=e["ordinal"])
            is_needed = (dll, name) in needed or (dll, name) in dyn
            if (dll in FULL and name in primary) or is_needed:
                out[(dll, name)] = dict(conv=e["conv"], pop=e["pop"], ordinal=e["ordinal"], default=0,
                                        needed=is_needed, src="wine:" + key)
    # 2. manual entries and decorated names
    man = os.path.join(HERE, "manual_sigs.txt")
    for l in open(man):
        if l.startswith("#") or not l.strip():
            continue
        f = l.rstrip("\n").split("\t")
        dll, name, conv, argb, dflt = f[:5]
        ordn = 0
        m = re.fullmatch(r"ord(\d+)", name)
        if m:
            ordn = int(m.group(1))
        out[(dll, name)] = dict(conv=conv, pop=int(argb) if conv in ("S", "T") else 0, ordinal=ordn, default=int(dflt, 0),
                                needed=(dll, name) in needed or (dll, name) in dyn, src="manual")
    # ordinal-only imports of xinput: add named aliases too (ord2 -> XInputGetState)
    for (dll, name), e in list(out.items()):
        pass
    # 3. everything in api_surface that still has no signature
    for (dll, name), cat in sorted(needed.items()):
        if (dll, name) in out:
            continue
        m = re.fullmatch(r"_?(\w+)@(\d+)", name)
        if m:  # stdcall-decorated (mss32, binkw32)
            out[(dll, name)] = dict(conv="S", pop=int(m.group(2)), ordinal=0, default=0, needed=True, src="decorated")
            continue
        out[(dll, name)] = dict(conv="?", pop=-1, ordinal=0, default=0, needed=True, src="MISSING")
    # nvcuda: ordinal-free names, all driver API calls are stdcall in CUDA 3.0 (arg sizes unknown -> report)
    for (dll, name), e in sorted(out.items()):
        if e["needed"] and (e["conv"] == "?" or e["src"] == "MISSING"):
            report.append(f"UNRESOLVED {dll}!{name} ({needed.get((dll,name),'dynamic')})")
    rows = sorted(out.items(), key=lambda kv: (kv[0][0], kv[0][1]))
    with open(OUT, "w") as fo:
        fo.write("// Generated by runtime/tools/gen_sigs.py from third_party/wine-specs + tools/analysis/out. Do not edit.\n")
        fo.write("// {dll, name, ordinal, conv, pop_bytes, default_ret, flags(1=needed)}\n")
        for (dll, name), e in rows:
            fo.write('{"%s", "%s", %d, \'%s\', %d, %d, %d},\n' % (dll, name.replace("\\", "\\\\"), e["ordinal"], e["conv"],
                                                          e["pop"], e["default"], 1 if e["needed"] else 0))
    with open(REPORT, "w") as fr:
        fr.write("\n".join(report) + "\n")
    print("wrote %s: %d entries, %d unresolved needed" % (OUT, len(rows), len(report)))
    for r in report[:60]:
        print(r)

main()
